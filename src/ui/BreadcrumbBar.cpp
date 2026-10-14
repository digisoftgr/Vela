#include "BreadcrumbBar.h"

#include <KLocalizedString>
#include <KShell>

#include <QDir>

#include <QAction>
#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QToolButton>

namespace {

// Horizontal padding inside a crumb button, on top of the text width.
constexpr int kCrumbPadding = 14;

// Splits a URL into the buttons that represent it.
//
// The first crumb is the root the rest hang off: "/" for a local path, and
// "scheme://host" for anything else, so a remote location still has somewhere to
// click back to. Every later crumb owns the URL of its own directory, which is
// what makes a click a plain navigation with no path arithmetic at the call site.
QList<QPair<QString, QUrl>> splitUrl(const QUrl &url)
{
    QList<QPair<QString, QUrl>> parts;
    if (!url.isValid())
        return parts;

    QUrl root = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    root.setPath(QStringLiteral("/"));

    if (url.isLocalFile()) {
        parts.append({QStringLiteral("/"), root});
    } else {
        const QString host = url.host();
        parts.append({host.isEmpty() ? url.scheme() : url.scheme() + QStringLiteral("://") + host,
                      root});
    }

    const QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString walked;
    for (const QString &segment : segments) {
        walked += QLatin1Char('/') + segment;
        QUrl segUrl = root;
        segUrl.setPath(walked);
        parts.append({segment, segUrl});
    }
    return parts;
}

} // namespace

BreadcrumbBar::BreadcrumbBar(QWidget *parent)
    : QWidget(parent)
{
    setCursor(Qt::IBeamCursor); // the dead space behaves like a text field
    setFocusPolicy(Qt::ClickFocus);

    m_overflowBtn = new QToolButton(this);
    m_overflowBtn->setAutoRaise(true);
    m_overflowBtn->setText(QStringLiteral("«"));
    m_overflowBtn->setToolTip(i18n("Show the rest of the path"));
    m_overflowBtn->setCursor(Qt::ArrowCursor);
    m_overflowBtn->hide();
    connect(m_overflowBtn, &QToolButton::clicked, this, &BreadcrumbBar::showOverflowMenu);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setPlaceholderText(i18n("Path…"));
    m_lineEdit->installEventFilter(this);
    m_lineEdit->hide();
    connect(m_lineEdit, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_lineEdit->text().trimmed();
        if (text.isEmpty())
            return;
        // "~", "~/foo" and names relative to the folder shown ("Documents",
        // "../x") resolve the way a shell would; taken as a bare QUrl they were
        // relative URLs with no scheme, and the navigation failed.
        const QString expanded = KShell::tildeExpand(text);
        const QString base = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
        QUrl target = QUrl::fromUserInput(expanded, base, QUrl::UserInputResolutionOption::AssumeLocalFile);
        if (target.isLocalFile())
            target = QUrl::fromLocalFile(QDir::cleanPath(target.toLocalFile()));
        // Leave edit mode first: the navigation that follows calls setUrl(),
        // and that must land on a bar already showing crumbs.
        leaveEditMode();
        if (target.isValid())
            Q_EMIT urlActivated(target);
    });
}

QSize BreadcrumbBar::sizeHint() const
{
    return m_lineEdit->sizeHint();
}

bool BreadcrumbBar::isEditing() const
{
    return m_lineEdit->isVisible();
}

QString BreadcrumbBar::pathText() const
{
    return m_url.isLocalFile() ? m_url.toLocalFile() : m_url.toString();
}

void BreadcrumbBar::setUrl(const QUrl &url)
{
    if (m_url == url)
        return;
    m_url = url;
    // A navigation while the field is open (a bookmark, the Up button, another
    // panel) makes whatever is half-typed there obsolete.
    if (isEditing())
        leaveEditMode();
    rebuildCrumbs();
    relayout();
}

void BreadcrumbBar::rebuildCrumbs()
{
    for (const Crumb &crumb : std::as_const(m_crumbs))
        delete crumb.button;
    m_crumbs.clear();

    const QList<QPair<QString, QUrl>> parts = splitUrl(m_url);
    m_crumbs.reserve(parts.size());
    for (const auto &[label, target] : parts) {
        Crumb crumb;
        crumb.label = label;
        crumb.full = label;
        crumb.url = target;
        crumb.button = new QToolButton(this);
        crumb.button->setAutoRaise(true);
        crumb.button->setText(label);
        crumb.button->setCursor(Qt::ArrowCursor);
        crumb.button->setFocusPolicy(Qt::NoFocus); // never a Tab stop
        crumb.button->installEventFilter(this);    // for middle-click
        crumb.button->hide();
        connect(crumb.button, &QToolButton::clicked, this, [this, target]() {
            // The last crumb is where we already are; re-navigating would throw
            // away the selection and scroll position for nothing.
            if (target.adjusted(QUrl::StripTrailingSlash)
                != m_url.adjusted(QUrl::StripTrailingSlash)) {
                Q_EMIT urlActivated(target);
            }
        });
        m_crumbs.append(crumb);
    }
}

void BreadcrumbBar::relayout()
{
    if (isEditing()) {
        m_lineEdit->setGeometry(rect());
        m_overflowBtn->hide();
        for (const Crumb &crumb : std::as_const(m_crumbs))
            crumb.button->hide();
        return;
    }

    const QFontMetrics fm(font());
    const int h = height();
    const int overflowW = fm.horizontalAdvance(m_overflowBtn->text()) + kCrumbPadding;

    // Fit backwards from the last crumb. Two passes are needed because the
    // overflow button only costs width once we know something was dropped, and
    // reserving it up front would drop a crumb that would otherwise have fitted.
    const int budget = width() - kTrailingClickWidth;
    int firstVisible = 0;
    int used = 0;
    for (int i = m_crumbs.size() - 1; i >= 0; --i) {
        const int w = fm.horizontalAdvance(m_crumbs.at(i).full) + kCrumbPadding
                    + (i > 0 ? kSeparatorWidth : 0);
        const bool needsOverflow = i > 0;
        if (i < m_crumbs.size() - 1 && used + w + (needsOverflow ? overflowW : 0) > budget) {
            firstVisible = i + 1;
            break;
        }
        used += w;
    }

    const bool overflowing = firstVisible > 0;
    m_overflowBtn->setVisible(overflowing);
    int x = 0;
    if (overflowing) {
        m_overflowBtn->setGeometry(x, 0, overflowW, h);
        x += overflowW;
    }

    for (int i = 0; i < m_crumbs.size(); ++i) {
        const Crumb &crumb = m_crumbs.at(i);
        if (i < firstVisible) {
            crumb.button->hide();
            continue;
        }
        if (i > firstVisible)
            x += kSeparatorWidth; // the gap the separator is painted into

        int w = fm.horizontalAdvance(crumb.full) + kCrumbPadding;
        // The last crumb is the one that must never disappear: if even it is too
        // wide, elide it rather than dropping it.
        const int remaining = width() - kTrailingClickWidth - x;
        if (w > remaining && remaining > 0) {
            w = remaining;
            crumb.button->setText(fm.elidedText(crumb.full, Qt::ElideMiddle, w - kCrumbPadding));
        } else {
            crumb.button->setText(crumb.full);
        }
        crumb.button->setToolTip(crumb.button->text() == crumb.full ? QString() : crumb.full);
        crumb.button->setGeometry(x, 0, w, h);
        crumb.button->show();
        x += w;
    }
}

void BreadcrumbBar::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    if (isEditing())
        return; // the field draws its own frame

    // Borrow the line edit's frame so the strip still reads as one editable
    // field. Without it the dead space to the right of the last crumb looks
    // like empty toolbar, and nothing suggests that clicking it does anything.
    {
        QPainter painter(this);
        QStyleOptionFrame option;
        option.initFrom(this);
        option.rect = rect();
        option.lineWidth = style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, this);
        option.midLineWidth = 0;
        option.state |= QStyle::State_Sunken;
        option.features = QStyleOptionFrame::None;
        style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &painter, this);
    }

    if (m_crumbs.size() < 2)
        return;

    // Separators live in the gaps relayout() left between buttons, so they never
    // widen a button's hover highlight or steal a click.
    QPainter painter(this);
    QColor colour = palette().color(QPalette::Disabled, QPalette::WindowText);
    painter.setPen(colour);
    for (int i = 1; i < m_crumbs.size(); ++i) {
        const QToolButton *button = m_crumbs.at(i).button;
        if (!button->isVisible())
            continue;
        const QRect gap(button->x() - kSeparatorWidth, 0, kSeparatorWidth, height());
        painter.drawText(gap, Qt::AlignCenter, QStringLiteral("›"));
    }
}

void BreadcrumbBar::mousePressEvent(QMouseEvent *event)
{
    // Only the dead space reaches here — clicks on crumbs are consumed by the
    // buttons themselves.
    if (event->button() == Qt::LeftButton) {
        enterEditMode();
        return;
    }
    QWidget::mousePressEvent(event);
}

void BreadcrumbBar::enterEditMode()
{
    if (isEditing()) {
        m_lineEdit->selectAll();
        return;
    }
    m_lineEdit->setText(pathText());
    m_overflowBtn->hide();
    for (const Crumb &crumb : std::as_const(m_crumbs))
        crumb.button->hide();
    m_lineEdit->setGeometry(rect());
    m_lineEdit->show();
    m_lineEdit->setFocus();
    // Selected on entry so the path can be copied with a single Ctrl+C.
    m_lineEdit->selectAll();
    update();
}

void BreadcrumbBar::leaveEditMode()
{
    if (!isEditing())
        return;
    m_lineEdit->hide();
    m_lineEdit->clear(); // edits are discarded, never carried into the next visit
    relayout();
    update();
}

void BreadcrumbBar::showOverflowMenu()
{
    QMenu menu(this);
    for (const Crumb &crumb : std::as_const(m_crumbs)) {
        if (crumb.button->isVisible())
            break; // the hidden ones are exactly the leading run
        QAction *action = menu.addAction(crumb.full);
        const QUrl target = crumb.url;
        connect(action, &QAction::triggered, this, [this, target]() {
            Q_EMIT urlActivated(target);
        });
    }
    if (menu.isEmpty())
        return;
    menu.exec(m_overflowBtn->mapToGlobal(QPoint(0, m_overflowBtn->height())));
}

void BreadcrumbBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool BreadcrumbBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_lineEdit) {
        if (event->type() == QEvent::FocusOut) {
            // Clicking away abandons the edit, so the bar never sits showing a
            // path that isn't where the panel is.
            leaveEditMode();
        } else if (event->type() == QEvent::KeyPress
                   && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            leaveEditMode();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    // Middle-click a crumb to open that directory in a new tab. QToolButton only
    // reports left clicks, so the button press has to be read directly.
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            for (const Crumb &crumb : std::as_const(m_crumbs)) {
                if (crumb.button == watched && crumb.button->rect().contains(me->pos())) {
                    Q_EMIT urlActivatedInNewTab(crumb.url);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
