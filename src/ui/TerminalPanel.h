#pragma once

#include <QPointer>
#include <QUrl>
#include <QWidget>

#include <utility>

class KJob;
class KMessageWidget;
class QAction;
class QKeyEvent;
class QResizeEvent;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;
class TerminalInterface;

namespace KParts {
class ReadOnlyPart;
}

// The embedded terminal at the bottom of the panels (F4), one per window,
// following the active panel — Dolphin's model, on Konsole's KPart.
//
// The shell lives only while the panel is shown: stopShell() ends it, along
// with anything running in it, and the next show starts a fresh one in the
// active panel's folder.
//
// Sync runs both ways (explorer → `cd` in the shell, `cd` in the shell →
// explorer), but only while the shell is provably sitting at its own prompt.
// That is decided from /proc, on an allowlist: the terminal's foreground
// process must be a known interactive shell, owned by this user, with a
// readable working directory and no job of its own running. Anything else
// pauses sync, and the category only picks the message in the header (ssh,
// multiplexer, other user, container, a program).
//
// Explorer changes that arrive while sync is paused are not replayed when the
// shell comes back: the header offers [Sync now] instead. A `cd` the shell made
// meanwhile does move the explorer, unless the explorer moved too (then the
// two disagree, and [Sync now] decides).
class TerminalPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalPanel(QWidget *parent = nullptr);
    ~TerminalPanel() override;

    // Starts a shell in the explorer's folder (home if it has no local path).
    // No-op while one is running. Shows an install hint without Konsole.
    void startShell();
    // Ends the shell and every process in it. Does not emit shellExited().
    void stopShell();
    bool hasShell() const { return m_part != nullptr; }

    // The active panel's folder. Called on navigation and panel switches.
    void setExplorerUrl(const QUrl &url);

    bool syncEnabled() const { return m_syncEnabled; }
    void setSyncEnabled(bool enabled);

    // A device under `mountpoint` is about to go away. A shell at its prompt
    // inside it is moved to `fallbackDir`, sync or not; a busy one cannot be,
    // and the header says why the unmount may fail.
    void relocateOffMount(const QString &mountpoint, const QString &fallbackDir);

    // Window actions whose shortcuts must reach the window even while the
    // terminal has focus. Konsole keeps single-modifier keys like F4 otherwise.
    void setPassthroughActions(const QList<QAction *> &actions);

    void focusTerminal();

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

Q_SIGNALS:
    // The user changed directory in the shell; the explorer should follow.
    void navigateRequested(const QUrl &url);
    // The shell's prompt hook fired: a command just finished, so folders it
    // touched (mkdir, rm, …) may have changed. Only with shell integration.
    void promptReturned();
    // The shell ended by itself (`exit`, Ctrl+D). The panel should hide.
    void shellExited();
    void syncEnabledChanged(bool enabled);

private Q_SLOTS:
    // Old-style slots: Konsole's part exposes these signals only by name.
    void onPartDirectoryChanged(const QString &dir);
    void onOverrideShortcut(QKeyEvent *event, bool &override);

private:
    // What the terminal's foreground is doing, as far as sync is concerned.
    enum class State {
        NoShell,      // not started, exited, or Konsole missing
        AtPrompt,     // a local shell of ours is waiting for input
        Busy,         // some program owns the terminal
        Ssh,          // a remote session: nothing it reports is local
        Multiplexer,  // tmux, screen, …: typing goes to whichever pane is active
        ForeignUser,  // sudo -s, su, or anything not running as us
        Container,    // distrobox, toolbox, docker exec, …: paths may differ
    };
    struct Probe {
        State   state = State::NoShell;
        QString cwd;      // the prompt shell's working directory (AtPrompt only)
        QString program;  // what is in the foreground otherwise, for the header
    };

    Probe probe() const;
    void refresh();            // probe, follow a shell-side cd, update the header
    void updateHeader();
    void sendCd(const QString &dir);
    bool consumeOwnCd(const QString &cwd);
    bool canCdNow() const;
    void resolveExplorerDir();
    void applyExplorerDir(const QString &dir);
    void onMostLocalUrl(KJob *job);
    void onPartDestroyed();

    QVBoxLayout *m_layout = nullptr;

    // ── Header ────────────────────────────────────────────────────────────────
    QWidget     *m_header      = nullptr;
    QLabel      *m_cwdLabel    = nullptr;
    QLabel      *m_statusLabel = nullptr;
    QToolButton *m_syncNowBtn  = nullptr;
    QToolButton *m_syncBtn     = nullptr;

    // ── Terminal ──────────────────────────────────────────────────────────────
    QPointer<KParts::ReadOnlyPart> m_part;
    TerminalInterface *m_terminal = nullptr;   // m_part, as its interface
    // Read once the shell runs. Everything about its state comes from /proc
    // through this pid, not from the part: the part asserts on a session that
    // is gone, and it outlives its shell until its own deleteLater() runs.
    int                m_shellPid = 0;
    KMessageWidget    *m_missingKonsole = nullptr;
    QTimer            *m_pollTimer = nullptr;
    QList<QPointer<QAction>> m_passthrough;

    // ── Sync state ────────────────────────────────────────────────────────────
    bool    m_syncEnabled = true;
    QUrl    m_explorerUrl;
    QString m_explorerDir;          // canonical local path; empty when remote
    QPointer<KJob> m_mostLocalJob;  // resolving a non-file URL to a local path
    QString m_shellCwd;             // last cwd seen at the prompt
    Probe   m_last;                 // last probe, for the header
    // The explorer moved while the shell could not follow. Cleared by the next
    // cd we send; while set, a cd made in the shell does not drag the explorer.
    bool    m_explorerPending = false;
    // `cd`s we typed, so their echo is not mistaken for the user's. Each entry
    // expires: a cd that failed (no permission) never produces its echo.
    QList<std::pair<QString, qint64>> m_ownCds;
    QString m_busyMount;            // mountpoint a busy shell is holding
    QString m_busyFallback;         // where to move it once the prompt returns
};
