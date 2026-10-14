#include "TerminalPanel.h"

#include <KColorScheme>
#include <KIO/DesktopExecParser>
#include <KIO/StatJob>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KProtocolInfo>
#include <KShell>
#include <kde_terminal_interface.h>

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <sys/stat.h>
#include <unistd.h>

// ── /proc probing ─────────────────────────────────────────────────────────────

namespace {

constexpr int kPollMs = 500;
constexpr qint64 kOwnCdTtlMs = 3000;
constexpr int kMaxTreeSize = 256; // a runaway fork tree must not stall the UI

struct ProcStat {
    bool ok = false;
    int pgrp = 0;
    int tpgid = 0; // foreground process group of the controlling terminal
};

ProcStat readStat(int pid)
{
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = f.readAll();
    // comm may contain spaces and parentheses; the fields start after the last ')'.
    const int close = data.lastIndexOf(')');
    if (close < 0)
        return {};
    // state ppid pgrp session tty_nr tpgid …
    const QList<QByteArray> fields = data.mid(close + 2).split(' ');
    if (fields.size() < 6)
        return {};
    return {true, fields.at(2).toInt(), fields.at(5).toInt()};
}

QStringList readCmdline(int pid)
{
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList args;
    const QList<QByteArray> parts = f.readAll().split('\0');
    for (const QByteArray &part : parts)
        args.append(QString::fromLocal8Bit(part));
    while (!args.isEmpty() && args.constLast().isEmpty())
        args.removeLast();
    return args;
}

// argv[0]'s basename, so `/usr/bin/ssh` and a login shell's `-bash` both read
// plainly. comm is the fallback: it is truncated to 15 characters.
QString processName(int pid)
{
    const QStringList args = readCmdline(pid);
    QString name = args.isEmpty() ? QString() : args.constFirst().section(QLatin1Char('/'), -1);
    if (name.startsWith(QLatin1Char('-')))
        name.remove(0, 1);
    if (name.isEmpty()) {
        QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
        if (f.open(QIODevice::ReadOnly))
            name = QString::fromLocal8Bit(f.readAll()).trimmed();
    }
    return name;
}

QList<int> childrenOf(int pid)
{
    QList<int> result;
    const QStringList tasks = QDir(QStringLiteral("/proc/%1/task").arg(pid))
                                  .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &tid : tasks) {
        QFile f(QStringLiteral("/proc/%1/task/%2/children").arg(pid).arg(tid));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QList<QByteArray> ids = f.readAll().simplified().split(' ');
        for (const QByteArray &id : ids) {
            if (const int child = id.toInt(); child > 0)
                result.append(child);
        }
    }
    return result;
}

// Every process below `root`, breadth first, capped.
QList<int> descendantsOf(int root)
{
    QList<int> all;
    QList<int> queue = childrenOf(root);
    while (!queue.isEmpty() && all.size() < kMaxTreeSize) {
        const int pid = queue.takeFirst();
        all.append(pid);
        queue.append(childrenOf(pid));
    }
    return all;
}

bool ownedByUs(int pid)
{
    struct stat st;
    const QByteArray path = QStringLiteral("/proc/%1").arg(pid).toLocal8Bit();
    return ::stat(path.constData(), &st) == 0 && st.st_uid == ::getuid();
}

// Already canonical: the kernel resolves symlinks. Empty when unreadable
// (another user's process) or when the directory has been deleted.
QString readCwd(int pid)
{
    const QString target = QFile::symLinkTarget(QStringLiteral("/proc/%1/cwd").arg(pid));
    if (target.endsWith(QLatin1String(" (deleted)")))
        return {};
    return target;
}

const QSet<QString> &shellNames()
{
    static const QSet<QString> names = {
        QStringLiteral("bash"), QStringLiteral("sh"),   QStringLiteral("dash"),
        QStringLiteral("zsh"),  QStringLiteral("fish"), QStringLiteral("ksh"),
        QStringLiteral("mksh"), QStringLiteral("oksh"), QStringLiteral("tcsh"),
        QStringLiteral("csh"),  QStringLiteral("nu"),   QStringLiteral("elvish"),
        QStringLiteral("xonsh"), QStringLiteral("yash"), QStringLiteral("ion"),
    };
    return names;
}

const QSet<QString> &sshNames()
{
    static const QSet<QString> names = {
        QStringLiteral("ssh"),     QStringLiteral("mosh"),   QStringLiteral("mosh-client"),
        QStringLiteral("autossh"), QStringLiteral("et"),     QStringLiteral("telnet"),
        QStringLiteral("rsh"),     QStringLiteral("rlogin"), QStringLiteral("tsh"),
    };
    return names;
}

const QSet<QString> &multiplexerNames()
{
    static const QSet<QString> names = {
        QStringLiteral("tmux"), QStringLiteral("screen"), QStringLiteral("zellij"),
        QStringLiteral("byobu"), QStringLiteral("dtach"), QStringLiteral("abduco"),
    };
    return names;
}

const QSet<QString> &containerNames()
{
    static const QSet<QString> names = {
        QStringLiteral("distrobox"),      QStringLiteral("distrobox-enter"),
        QStringLiteral("toolbox"),        QStringLiteral("docker"),
        QStringLiteral("podman"),         QStringLiteral("nerdctl"),
        QStringLiteral("kubectl"),        QStringLiteral("lxc"),
        QStringLiteral("lxc-attach"),     QStringLiteral("incus"),
        QStringLiteral("systemd-nspawn"), QStringLiteral("machinectl"),
        QStringLiteral("chroot"),         QStringLiteral("arch-chroot"),
        QStringLiteral("schroot"),        QStringLiteral("flatpak-spawn"),
    };
    return names;
}

const QSet<QString> &elevationNames()
{
    static const QSet<QString> names = {
        QStringLiteral("sudo"), QStringLiteral("sudo-rs"), QStringLiteral("su"),
        QStringLiteral("doas"), QStringLiteral("pkexec"),  QStringLiteral("run0"),
    };
    return names;
}

// A shell reading commands from the terminal, not running a script or `-c`.
// `bash --rcfile x` (nix-shell) is interactive; `bash build.sh` is not.
bool isInteractiveShell(int pid)
{
    const QStringList args = readCmdline(pid);
    if (args.isEmpty())
        return false;
    QString name = args.constFirst().section(QLatin1Char('/'), -1);
    if (name.startsWith(QLatin1Char('-')))
        name.remove(0, 1);
    if (!shellNames().contains(name))
        return false;

    static const QSet<QString> optionsWithValue = {
        QStringLiteral("--rcfile"), QStringLiteral("--init-file"), QStringLiteral("-o"),
        QStringLiteral("-O"), QStringLiteral("+O"), QStringLiteral("-C"),
        QStringLiteral("--init-command"),
    };
    bool forced = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("-c") || a == QLatin1String("--command")
            || a.startsWith(QLatin1String("--command=")))
            return false;
        if (a == QLatin1String("-i") || a == QLatin1String("--interactive")) {
            forced = true;
            continue;
        }
        if (optionsWithValue.contains(a)) {
            ++i;
            continue;
        }
        if (a == QLatin1String("--"))
            return i + 1 >= args.size() || forced;
        if (a.startsWith(QLatin1String("--")))
            continue;
        if (a.startsWith(QLatin1Char('-')) || a.startsWith(QLatin1Char('+'))) {
            // Bundled short flags: `bash -lc 'cmd'` runs a command.
            if (a.size() > 1 && a.at(0) == QLatin1Char('-') && a.contains(QLatin1Char('c')))
                return false;
            continue;
        }
        return forced; // a script path
    }
    return true;
}

QString canonicalDir(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

bool isUnder(const QString &path, const QString &dir)
{
    return path == dir || path.startsWith(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/'));
}


// ── Shell integration ─────────────────────────────────────────────────────────
//
// The shell is started with a hook that prints OSC 7 (its folder) at every
// prompt. Konsole turns that into currentDirectoryChanged within ~20 ms, so a
// cd shows in the explorer at once instead of at the next poll. The OSC 7 path
// itself is never trusted, only taken as "look now": /proc stays the truth, so
// a remote or misbehaving shell cannot steer the explorer.
//
// The user's dotfiles are not touched: Vela's own rc files, written to the
// runtime dir on each start, source the usual ones and then add the hook.

const char kBashRc[] = R"vela(# Written by Vela for its terminal panel; recreated on every start.
# Your usual ~/.bashrc, then a prompt hook reporting the folder (OSC 7).
# (/etc/bash.bashrc has already run: --rcfile does not skip it.)
if [ -f ~/.bashrc ]; then . ~/.bashrc; fi
__vela_osc7() { local s=$?; printf '\033]7;file://%s%s\033\\' "$HOSTNAME" "$PWD"; return $s; }
if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
    PROMPT_COMMAND+=(__vela_osc7)
else
    __vela_pc=$PROMPT_COMMAND
    while [[ $__vela_pc == *[[:space:]\;] ]]; do __vela_pc=${__vela_pc%?}; done
    PROMPT_COMMAND="${__vela_pc:+$__vela_pc; }__vela_osc7"
fi
unset __vela_pc
)vela";

// zsh reads $ZDOTDIR/.zshenv and .zshrc. Vela points ZDOTDIR here; each file
// sources the user's counterpart with the user's ZDOTDIR in place, which their
// .zshenv may itself change.
const char kZshEnv[] = R"vela(# Written by Vela for its terminal panel; recreated on every start.
__vela_zdotdir=$ZDOTDIR
ZDOTDIR=${VELA_USER_ZDOTDIR:-$HOME}
[[ -f $ZDOTDIR/.zshenv ]] && source $ZDOTDIR/.zshenv
VELA_USER_ZDOTDIR=$ZDOTDIR
ZDOTDIR=$__vela_zdotdir
unset __vela_zdotdir
)vela";

const char kZshRc[] = R"vela(# Written by Vela for its terminal panel; recreated on every start.
# Your usual .zshrc, then a precmd hook reporting the folder (OSC 7).
ZDOTDIR=$VELA_USER_ZDOTDIR
[[ -f $ZDOTDIR/.zshrc ]] && source $ZDOTDIR/.zshrc
[[ -z $VELA_HAD_ZDOTDIR && $ZDOTDIR == $HOME ]] && unset ZDOTDIR
unset VELA_USER_ZDOTDIR VELA_HAD_ZDOTDIR
__vela_osc7() { printf '\e]7;file://%s%s\e\\' "$HOST" "$PWD" }
autoload -Uz add-zsh-hook && add-zsh-hook precmd __vela_osc7
)vela";

const char kFishInit[] =
    "function __vela_osc7 --on-event fish_prompt; "
    "printf '\\e]7;file://%s%s\\e\\\\' $hostname $PWD; end";

// Writes `content` to `path` unless it is already there.
bool writeIfChanged(const QString &path, const QByteArray &content)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.readAll() == content)
        return true;
    f.close();
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(content) == content.size();
}

// The argv (argv[0] included, Konsole's convention) that starts `shell` in
// `dir` with the hook, or empty when the shell is not one we integrate.
// sh does the cd, as startProgram() takes no working directory, then execs
// the shell — same pid, so the /proc logic sees the shell itself.
QStringList integratedLaunch(const QString &shell, const QString &dir)
{
    const QString name = shell.section(QLatin1Char('/'), -1);
    if (name != QLatin1String("bash") && name != QLatin1String("zsh")
        && name != QLatin1String("fish"))
        return {};

    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        return {};
    const QString root = base + QStringLiteral("/vela/shell-integration");
    if (!QDir().mkpath(root + QStringLiteral("/zsh")))
        return {};

    const QString cd = QStringLiteral("cd -- \"$1\" 2>/dev/null || cd; ");
    const QString sh = QStringLiteral("/bin/sh");
    if (name == QLatin1String("bash")) {
        const QString rc = root + QStringLiteral("/bash.rc");
        if (!writeIfChanged(rc, kBashRc))
            return {};
        return {sh, QStringLiteral("-c"), cd + QStringLiteral("exec \"$2\" --rcfile \"$3\" -i"),
                QStringLiteral("sh"), dir, shell, rc};
    }
    if (name == QLatin1String("zsh")) {
        if (!writeIfChanged(root + QStringLiteral("/zsh/.zshenv"), kZshEnv)
            || !writeIfChanged(root + QStringLiteral("/zsh/.zshrc"), kZshRc))
            return {};
        return {sh, QStringLiteral("-c"),
                cd + QStringLiteral("VELA_HAD_ZDOTDIR=${ZDOTDIR+1} "
                                    "VELA_USER_ZDOTDIR=${ZDOTDIR:-$HOME} "
                                    "ZDOTDIR=\"$3\" exec \"$2\" -i"),
                QStringLiteral("sh"), dir, shell, root + QStringLiteral("/zsh")};
    }
    return {sh, QStringLiteral("-c"), cd + QStringLiteral("exec \"$2\" --init-command \"$3\""),
            QStringLiteral("sh"), dir, shell, QString::fromLatin1(kFishInit)};
}

} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

TerminalPanel::TerminalPanel(QWidget *parent)
    : QWidget(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // Header: [shell cwd] [status] … [Sync now] [Sync]
    m_header = new QWidget(this);
    auto *row = new QHBoxLayout(m_header);
    row->setContentsMargins(6, 1, 4, 1);
    row->setSpacing(4);

    m_cwdLabel = new QLabel(m_header);
    // Ignored: the path is elided to whatever width is left, never widening the window.
    m_cwdLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_cwdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(m_cwdLabel, 2);

    m_statusLabel = new QLabel(m_header);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_statusLabel, 3);

    const auto makeButton = [this, row](const QString &icon, const QString &tip) {
        auto *btn = new QToolButton(m_header);
        btn->setAutoRaise(true);
        btn->setIcon(QIcon::fromTheme(icon));
        btn->setToolTip(tip);
        row->addWidget(btn);
        return btn;
    };

    m_syncNowBtn = makeButton(QStringLiteral("view-refresh"),
                              i18n("Move the terminal to the explorer's folder"));
    m_syncNowBtn->setText(i18n("Sync Now"));
    m_syncNowBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_syncNowBtn->hide();
    connect(m_syncNowBtn, &QToolButton::clicked, this, [this]() {
        m_last = probe();
        if (m_last.state == State::AtPrompt && !m_explorerDir.isEmpty())
            sendCd(m_explorerDir);
        updateHeader();
    });

    m_syncBtn = makeButton(QStringLiteral("link"), QString());
    m_syncBtn->setText(i18n("Sync"));
    m_syncBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_syncBtn->setCheckable(true);
    m_syncBtn->setChecked(m_syncEnabled);
    connect(m_syncBtn, &QToolButton::toggled, this, &TerminalPanel::setSyncEnabled);

    m_layout->addWidget(m_header);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &TerminalPanel::refresh);

    updateHeader();
}

TerminalPanel::~TerminalPanel()
{
    // Before QObject child cleanup, and without shellExited(): the window is
    // already going away.
    stopShell();
}

// ── Shell lifetime ────────────────────────────────────────────────────────────

void TerminalPanel::startShell()
{
    if (m_part)
        return;

    const KPluginFactory::Result<KPluginFactory> loaded =
        KPluginFactory::loadFactory(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")));
    KParts::ReadOnlyPart *part =
        loaded.plugin ? loaded.plugin->create<KParts::ReadOnlyPart>(this) : nullptr;
    TerminalInterface *terminal = part ? qobject_cast<TerminalInterface *>(part) : nullptr;
    if (!terminal) {
        delete part;
        if (!m_missingKonsole) {
            m_missingKonsole = new KMessageWidget(
                i18n("The terminal cannot be shown because Konsole is not installed. "
                     "Install it, then open the terminal again."),
                this);
            m_missingKonsole->setMessageType(KMessageWidget::Information);
            m_missingKonsole->setCloseButtonVisible(false);
            m_missingKonsole->setWordWrap(true);
            const QUrl installUrl(QStringLiteral("appstream://org.kde.konsole.desktop"));
            if (KIO::DesktopExecParser::hasSchemeHandler(installUrl)) {
                auto *install = new QAction(i18n("Install Konsole"), m_missingKonsole);
                connect(install, &QAction::triggered, this, [installUrl]() {
                    QDesktopServices::openUrl(installUrl);
                });
                m_missingKonsole->addAction(install);
            }
            m_layout->addWidget(m_missingKonsole);
            m_layout->addStretch(1);
        }
        m_missingKonsole->show();
        updateHeader();
        return;
    }
    if (m_missingKonsole)
        m_missingKonsole->hide();

    m_part = part;
    m_terminal = terminal;
    // The part deletes itself when its shell exits.
    connect(part, &QObject::destroyed, this, &TerminalPanel::onPartDestroyed);
    QWidget *view = part->widget();
    m_layout->addWidget(view, 1);
    setFocusProxy(view);

    // String-based: Konsole's part is a plugin, its signals are not in any
    // header we can include.
    connect(part, SIGNAL(currentDirectoryChanged(QString)),
            this, SLOT(onPartDirectoryChanged(QString)));
    connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)),
            this, SLOT(onOverrideShortcut(QKeyEvent *, bool &)));

    m_shellCwd.clear();
    m_ownCds.clear();
    m_explorerPending = false;
    m_busyMount.clear();
    m_last = {};

    const QString dir = m_explorerDir.isEmpty() ? QDir::homePath() : m_explorerDir;
    // Integrate only a plain shell: a profile with its own arguments (`zsh -l`,
    // a custom program) runs exactly as configured, and falls back to polling.
    const QStringList command = KShell::splitArgs(
        m_terminal->profileProperty(QStringLiteral("Command")).toString());
    const QStringList profileArgs =
        m_terminal->profileProperty(QStringLiteral("Arguments")).toStringList();
    QStringList argv;
    if (command.size() == 1 && profileArgs.size() <= 1) {
        const QString shell = command.constFirst().startsWith(QLatin1Char('/'))
                                  ? command.constFirst()
                                  : QStandardPaths::findExecutable(command.constFirst());
        if (!shell.isEmpty())
            argv = integratedLaunch(shell, dir);
    }
    if (argv.isEmpty())
        m_terminal->showShellInDir(dir);
    else
        m_terminal->startProgram(argv.constFirst(), argv);
    m_shellPid = m_terminal->terminalProcessId();
    m_pollTimer->start();
    refresh();
}

void TerminalPanel::stopShell()
{
    m_pollTimer->stop();
    m_terminal = nullptr;
    m_shellPid = 0;
    m_last = {};
    if (KParts::ReadOnlyPart *part = m_part.data()) {
        disconnect(part, &QObject::destroyed, this, &TerminalPanel::onPartDestroyed);
        m_part.clear();
        // Konsole hangs up the pty: the shell and its foreground job get
        // SIGHUP, and the shell passes it on to its other jobs.
        delete part;
    }
    updateHeader();
}

void TerminalPanel::onPartDestroyed()
{
    m_pollTimer->stop();
    m_terminal = nullptr;
    m_shellPid = 0;
    m_last = {};
    updateHeader();
    Q_EMIT shellExited();
}

// ── State ─────────────────────────────────────────────────────────────────────

TerminalPanel::Probe TerminalPanel::probe() const
{
    if (!m_part || m_shellPid <= 0)
        return {};
    const ProcStat shell = readStat(m_shellPid);
    if (!shell.ok)
        return {}; // exited; the part's deleteLater() has not run yet

    int target = m_shellPid;
    const int fg = shell.tpgid;
    if (fg > 0 && fg != shell.pgrp) {
        // Members of the foreground group (its leader may have exited, as in
        // a pipeline whose first command finished), plus everything they
        // started: `gcloud compute ssh` is Python with ssh below it.
        const QList<int> tree = descendantsOf(m_shellPid);
        QList<int> group;
        for (int pid : tree) {
            if (readStat(pid).pgrp == fg)
                group.append(pid);
        }
        if (group.isEmpty())
            return {State::Busy, {}, QString()};
        QList<int> involved = group;
        for (int pid : std::as_const(group)) {
            for (int child : descendantsOf(pid)) {
                if (!involved.contains(child))
                    involved.append(child);
            }
        }
        const int leader = group.contains(fg) ? fg : group.constFirst();
        const QString leaderName = processName(leader);

        // Each name is read from /proc once per poll. Matching each name set
        // in turn used to re-read every process's cmdline up to four times,
        // every 500 ms, on the GUI thread — for a build, hundreds of processes.
        QStringList involvedNames;
        involvedNames.reserve(involved.size());
        for (int pid : std::as_const(involved))
            involvedNames.append(processName(pid));
        const auto anyIn = [&involvedNames](const QSet<QString> &names) -> QString {
            for (const QString &name : std::as_const(involvedNames)) {
                if (names.contains(name))
                    return name;
            }
            return {};
        };
        if (const QString n = anyIn(sshNames()); !n.isEmpty())
            return {State::Ssh, {}, n};
        if (const QString n = anyIn(multiplexerNames()); !n.isEmpty())
            return {State::Multiplexer, {}, n};
        if (const QString n = anyIn(containerNames()); !n.isEmpty())
            return {State::Container, {}, n};
        if (const QString n = anyIn(elevationNames()); !n.isEmpty())
            return {State::ForeignUser, {}, n};
        if (!ownedByUs(leader))
            return {State::ForeignUser, {}, leaderName};

        // A nested local shell at its own prompt still syncs. It owns the
        // terminal alone: a job it runs either gets its own group (job
        // control) or, in a script, shares this one.
        if (group.size() > 1 || !isInteractiveShell(leader))
            return {State::Busy, {}, leaderName};
        target = leader;
    }

    const QString cwd = readCwd(target);
    if (cwd.isEmpty())
        return {ownedByUs(target) ? State::Busy : State::ForeignUser, {}, processName(target)};
    return {State::AtPrompt, cwd, QString()};
}

void TerminalPanel::refresh()
{
    if (m_part && m_shellPid <= 0 && m_terminal)
        m_shellPid = m_terminal->terminalProcessId();

    const Probe p = probe();
    if (p.state == State::AtPrompt) {
        if (!m_busyMount.isEmpty()) {
            // The program holding the device is gone; finish the move now.
            if (isUnder(p.cwd, m_busyMount))
                sendCd(m_busyFallback);
            m_busyMount.clear();
        }

        if (p.cwd != m_shellCwd) {
            const bool firstSight = m_shellCwd.isEmpty();
            m_shellCwd = p.cwd;
            if (p.cwd == m_explorerDir)
                m_explorerPending = false;
            const bool ours = consumeOwnCd(p.cwd);
            // The user moved the shell: the explorer follows, unless it has a
            // move of its own the shell never took (then the two disagree, and
            // [Sync now] decides), or it shows a remote folder.
            if (!firstSight && !ours && m_syncEnabled && !m_explorerPending
                && !m_explorerDir.isEmpty() && p.cwd != m_explorerDir)
                Q_EMIT navigateRequested(QUrl::fromLocalFile(p.cwd));
        }
    }
    m_last = p;
    updateHeader();
}

bool TerminalPanel::canCdNow() const
{
    return m_terminal && m_last.state == State::AtPrompt;
}

void TerminalPanel::sendCd(const QString &dir)
{
    if (!m_terminal || dir.isEmpty())
        return;
    // Ctrl+E, Ctrl+U: empty the line first. Without it, `cd x⏎` appended to a
    // half-typed `rm -rf *` would run it (Dolphin learned this the hard way).
    m_terminal->sendInput(QStringLiteral("\x05\x15"));
    // The leading space keeps it out of history where HISTCONTROL/HIST_IGNORE_SPACE allow.
    m_terminal->sendInput(QStringLiteral(" cd ") + KShell::quoteArg(dir) + QLatin1Char('\r'));
    m_ownCds.append({dir, QDateTime::currentMSecsSinceEpoch()});
    m_explorerPending = false;
}

bool TerminalPanel::consumeOwnCd(const QString &cwd)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!m_ownCds.isEmpty()) {
        const auto [dir, sentAt] = m_ownCds.takeFirst();
        if (now - sentAt > kOwnCdTtlMs)
            continue;
        if (dir == cwd)
            return true;
    }
    return false;
}

// ── Explorer side ─────────────────────────────────────────────────────────────

void TerminalPanel::setExplorerUrl(const QUrl &url)
{
    if (url == m_explorerUrl)
        return;
    m_explorerUrl = url;
    resolveExplorerDir();
}

void TerminalPanel::resolveExplorerDir()
{
    if (m_mostLocalJob)
        m_mostLocalJob->kill();

    const QUrl url = m_explorerUrl;
    if (!url.isValid()) {
        applyExplorerDir(QString());
        return;
    }
    if (url.isLocalFile()) {
        applyExplorerDir(canonicalDir(url.toLocalFile()));
        return;
    }
    // desktop:/, and other ":local" protocols, have a local path behind them.
    if (KProtocolInfo::protocolClass(url.scheme()) == QLatin1String(":local")) {
        m_explorerDir.clear();
        KIO::StatJob *job = KIO::mostLocalUrl(url, KIO::HideProgressInfo);
        m_mostLocalJob = job;
        connect(job, &KJob::result, this, &TerminalPanel::onMostLocalUrl);
        updateHeader();
        return;
    }
    applyExplorerDir(QString()); // remote: sync pauses
}

void TerminalPanel::onMostLocalUrl(KJob *job)
{
    auto *statJob = static_cast<KIO::StatJob *>(job);
    if (job != m_mostLocalJob)
        return;
    const QUrl local = statJob->error() ? QUrl() : statJob->mostLocalUrl();
    applyExplorerDir(local.isLocalFile() ? canonicalDir(local.toLocalFile()) : QString());
}

void TerminalPanel::applyExplorerDir(const QString &dir)
{
    m_explorerDir = dir;
    if (dir.isEmpty() || !m_part) {
        updateHeader();
        return;
    }
    m_last = probe();
    if (dir == m_shellCwd && m_last.state == State::AtPrompt) {
        m_explorerPending = false;
    } else if (m_syncEnabled) {
        if (m_last.state == State::AtPrompt)
            sendCd(dir);
        else
            m_explorerPending = true; // not replayed later: [Sync now] offers it
    }
    updateHeader();
}

// ── Sync toggle ───────────────────────────────────────────────────────────────

void TerminalPanel::setSyncEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;
    m_syncEnabled = enabled;
    {
        const QSignalBlocker block(m_syncBtn);
        m_syncBtn->setChecked(enabled);
    }
    // A fresh start: what happened while off is not held against either side.
    m_explorerPending = false;
    updateHeader();
    Q_EMIT syncEnabledChanged(enabled);
}

// ── Unmount ───────────────────────────────────────────────────────────────────

void TerminalPanel::relocateOffMount(const QString &mountpoint, const QString &fallbackDir)
{
    if (!m_part || mountpoint.isEmpty())
        return;
    const QString mount = QDir::cleanPath(mountpoint);
    m_last = probe();
    const QString cwd = m_last.state == State::AtPrompt ? m_last.cwd : m_shellCwd;
    if (cwd.isEmpty() || !isUnder(cwd, mount))
        return;
    if (m_last.state == State::AtPrompt) {
        // Regardless of sync: holding a device busy is never what was meant.
        // The cd is typed, so it races a teardown that is already under way.
        sendCd(fallbackDir);
    } else {
        m_busyMount = mount;
        m_busyFallback = fallbackDir;
    }
    updateHeader();
}

// ── Header ────────────────────────────────────────────────────────────────────

void TerminalPanel::updateHeader()
{
    // Shell cwd, home shortened to ~.
    QString cwd = m_shellCwd;
    const QString home = QDir::homePath();
    if (isUnder(cwd, home))
        cwd.replace(0, home.size(), QStringLiteral("~"));
    m_cwdLabel->setToolTip(m_shellCwd);
    m_cwdLabel->setText(m_part ? m_cwdLabel->fontMetrics().elidedText(
                                     cwd, Qt::ElideMiddle, qMax(m_cwdLabel->width(), 0))
                               : QString());

    // An own cd still on its way is not "out of sync" — it would flicker.
    bool cdInFlight = false;
    for (const auto &[dir, sentAt] : std::as_const(m_ownCds)) {
        if (dir == m_explorerDir)
            cdInFlight = true;
    }
    const bool outOfSync = m_part && m_syncEnabled && !m_explorerDir.isEmpty()
                           && !m_shellCwd.isEmpty() && m_shellCwd != m_explorerDir
                           && !cdInFlight;
    const bool remoteExplorer = m_explorerUrl.isValid() && m_explorerDir.isEmpty()
                                && !m_mostLocalJob;

    QString status;
    KColorScheme::ForegroundRole role = KColorScheme::NeutralText;
    if (!m_part) {
        status.clear();
    } else if (!m_busyMount.isEmpty()) {
        status = i18n("%1 is using %2: unmounting may fail",
                      m_last.program.isEmpty() ? i18n("A program") : m_last.program, m_busyMount);
        role = KColorScheme::NegativeText;
    } else if (!m_syncEnabled) {
        status = i18n("Sync off");
        role = KColorScheme::InactiveText;
    } else if (remoteExplorer) {
        status = i18n("Remote folder: sync paused");
    } else {
        switch (m_last.state) {
        case State::Ssh:
            status = i18n("SSH session: sync paused");
            break;
        case State::Multiplexer:
            status = i18n("%1: sync paused", m_last.program);
            break;
        case State::ForeignUser:
            status = i18n("Shell runs as another user: sync paused");
            break;
        case State::Container:
            status = i18n("Container (%1): sync paused", m_last.program);
            break;
        case State::Busy:
            if (outOfSync)
                status = m_last.program.isEmpty()
                             ? i18n("Program running: out of sync")
                             : i18n("%1 running: out of sync", m_last.program);
            break;
        case State::AtPrompt:
            if (outOfSync)
                status = i18n("Out of sync");
            break;
        case State::NoShell:
            break;
        }
    }

    QPalette pal = m_statusLabel->palette();
    pal.setColor(QPalette::WindowText,
                 KColorScheme(QPalette::Active, KColorScheme::Window).foreground(role).color());
    m_statusLabel->setPalette(pal);
    m_statusLabel->setText(status);
    m_statusLabel->setToolTip(status);

    m_syncNowBtn->setVisible(outOfSync);
    m_syncNowBtn->setEnabled(canCdNow());
    m_syncBtn->setToolTip(m_syncEnabled
                              ? i18n("Terminal and explorer follow each other (click to stop)")
                              : i18n("Terminal and explorer are independent (click to sync)"));
}

void TerminalPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateHeader();
    QWidget::changeEvent(event);
}

void TerminalPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateHeader(); // re-elide the path
}

// ── Focus and shortcuts ───────────────────────────────────────────────────────

void TerminalPanel::focusTerminal()
{
    if (m_part && m_part->widget())
        m_part->widget()->setFocus(Qt::ShortcutFocusReason);
}

void TerminalPanel::setPassthroughActions(const QList<QAction *> &actions)
{
    m_passthrough.clear();
    for (QAction *a : actions)
        m_passthrough.append(a);
}

void TerminalPanel::onOverrideShortcut(QKeyEvent *event, bool &override)
{
    const QKeySequence pressed(event->keyCombination());
    for (const QPointer<QAction> &action : std::as_const(m_passthrough)) {
        if (!action)
            continue;
        const QList<QKeySequence> shortcuts = action->shortcuts();
        for (const QKeySequence &shortcut : shortcuts) {
            if (shortcut.matches(pressed) == QKeySequence::ExactMatch) {
                override = false;
                return;
            }
        }
    }
}

void TerminalPanel::onPartDirectoryChanged(const QString &)
{
    // Konsole noticed first; /proc stays the source of truth.
    refresh();
    // With the prompt hook this arrives once per prompt (Konsole's own cwd
    // polling only reports actual changes, so it rarely adds to that).
    if (m_last.state == State::AtPrompt)
        Q_EMIT promptReturned();
}
