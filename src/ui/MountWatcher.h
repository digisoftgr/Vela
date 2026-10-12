#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace Solid {
class Device;
}

// Tells every window when a mounted filesystem is about to go away, or has
// gone, so panels showing it can move off it — the same thing Dolphin does.
//
// A panel left sitting on an unmounted device either keeps the busy mount from
// unmounting at all, or shows a stale listing: for a mountpoint that survives
// the unmount (an fstab entry such as /mnt/data) the empty directory is still
// there, so the "directory was deleted" checks never fire.
//
// Three triggers, most to least graceful:
//   * prepareTeardown(): our own sidebar is about to unmount — synchronous, so
//     the panels have let go before the request reaches the device;
//   * Solid's teardownRequested: another program (the Plasma device applet,
//     Dolphin) asked for the unmount;
//   * accessibility lost / device removed: it is already gone (udisksctl,
//     umount in a terminal, a pulled cable). Mountpoints are remembered while
//     mounted because Solid no longer reports one afterwards.
class MountWatcher : public QObject
{
    Q_OBJECT

public:
    static MountWatcher *self();

    // Called right before this process requests an unmount of `mountpoint`.
    void prepareTeardown(const QString &mountpoint);

Q_SIGNALS:
    // Delivered once per unmount event; handlers must tolerate repeats (a
    // teardown request is usually followed by the accessibility change).
    void mountpointLeaving(const QString &mountpoint);

private:
    MountWatcher();
    void track(const Solid::Device &device);
    void onAccessibilityChanged(bool accessible, const QString &udi);
    void onTeardownRequested(const QString &udi);

    QHash<QString, QString> m_mountpoints; // udi → mountpoint, while mounted
};
