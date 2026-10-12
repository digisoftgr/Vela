#include "MountWatcher.h"

#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/StorageAccess>

MountWatcher *MountWatcher::self()
{
    static MountWatcher *instance = new MountWatcher;
    return instance;
}

MountWatcher::MountWatcher()
{
    const QList<Solid::Device> devices =
        Solid::Device::listFromType(Solid::DeviceInterface::StorageAccess);
    for (const Solid::Device &device : devices)
        track(device);

    Solid::DeviceNotifier *notifier = Solid::DeviceNotifier::instance();
    connect(notifier, &Solid::DeviceNotifier::deviceAdded, this, [this](const QString &udi) {
        track(Solid::Device(udi));
    });
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved, this, [this](const QString &udi) {
        const QString mountpoint = m_mountpoints.take(udi);
        if (!mountpoint.isEmpty())
            Q_EMIT mountpointLeaving(mountpoint);
    });
}

void MountWatcher::track(const Solid::Device &device)
{
    // Non-const: Solid only hands out the interface from a mutable device.
    Solid::Device dev = device;
    auto *access = dev.as<Solid::StorageAccess>();
    if (!access)
        return;
    const QString udi = dev.udi();
    if (access->isAccessible() && !access->filePath().isEmpty())
        m_mountpoints.insert(udi, access->filePath());

    connect(access, &Solid::StorageAccess::accessibilityChanged,
            this, &MountWatcher::onAccessibilityChanged, Qt::UniqueConnection);
    // Member slots, not lambdas: Qt::UniqueConnection has no effect on
    // functors, and a device can be tracked twice (listed, then announced).
    connect(access, &Solid::StorageAccess::teardownRequested,
            this, &MountWatcher::onTeardownRequested, Qt::UniqueConnection);
}

void MountWatcher::onTeardownRequested(const QString &udi)
{
    const QString mountpoint = m_mountpoints.value(udi);
    if (!mountpoint.isEmpty())
        Q_EMIT mountpointLeaving(mountpoint);
}

void MountWatcher::onAccessibilityChanged(bool accessible, const QString &udi)
{
    if (accessible) {
        Solid::Device dev(udi);
        if (auto *access = dev.as<Solid::StorageAccess>()) {
            if (!access->filePath().isEmpty())
                m_mountpoints.insert(udi, access->filePath());
        }
        return;
    }
    const QString mountpoint = m_mountpoints.take(udi);
    if (!mountpoint.isEmpty())
        Q_EMIT mountpointLeaving(mountpoint);
}

void MountWatcher::prepareTeardown(const QString &mountpoint)
{
    if (!mountpoint.isEmpty())
        Q_EMIT mountpointLeaving(mountpoint);
}
