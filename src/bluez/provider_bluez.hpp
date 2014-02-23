#ifndef _STATEFS_PRIVATE_BLUEZ_HPP_
#define _STATEFS_PRIVATE_BLUEZ_HPP_

#include "manager_interface.h"
#include "adapter_interface.h"
#include "device_interface.h"

#include <statefs/provider.hpp>
#include <statefs/property.hpp>
#include <statefs/qt/ns.hpp>
#include <set>

#include <QObject>

namespace statefs { namespace bluez {

typedef OrgBluezManagerInterface Manager;
typedef OrgBluezAdapterInterface Adapter;
typedef OrgBluezDeviceInterface Device;

class BlueZ;

enum class Protocol {
    Headset = 0, HIDP,
        EOE
};

template <typename T>
constexpr size_t size()
{
    return static_cast<size_t>(T::EOE);
}

struct DeviceMonitor
{
    virtual void onProtocolsAdded(QString const&, std::set<Protocol> const&) =0;
    virtual void onProtocolsRemoved(QString const&, std::set<Protocol> const&) =0;
    virtual void onStateChanged(QString const&, DeviceInfo::State) =0;
};

class DeviceInfo : public QObject
{
    Q_OBJECT;
public:

    enum class State {
        Unknown, Connected, Disconnected
    };

    DeviceInfo(const QString &service, const QString &path
               , const QDBusConnection &connection)
        : state_(State::Unknown)
        , device_(std::make_shared<Device>(service_name, path, bus_))
        , path_(path)
    {
        connect(device.get(), &Device::PropertyChanged
                , [this](const QString &name, const QDBusVariant &value) {
                    processProperty(name, value.variant());
                });
        async(device->GetProperties()
              , [this](const QVariantMap &props) {
                  for (auto it = props.begin(); it != props.end(); ++it)
                      processProperty(it.key(), it.value());
              });
    }

signals:

    void onProtocolsAdded(QString const&, std::set<Protocol> const&);
    void onProtocolsRemoved(QString const&, std::set<Protocol> const&);
    void onStateChanged(QString const&, DeviceInfo::State);

private:

    void processUUIDs(QVariant const &);
    void processConnected(bool);
    void processProperty(const QString &, const QVariant &);

    State state_;
    std::shared_ptr<Device> device_;
    QString path_;
    std::set<Protocol> protocols_;
};

class Devices : public DeviceMonitor
{
public:
    void add(const QDBusObjectPath &);
    void remove(const QDBusObjectPath &);
private:
    std::map<QString, std::shared_ptr<DeviceInfo> > devices_;
    std::set<QString> connected_;
    std::array<std::set<QString>, size<Protocol>()> connected_protocols_;
};

class Bridge : public QObject, public statefs::qt::PropertiesSource
{
    Q_OBJECT
public:

    Bridge(BlueZ *, QDBusConnection &);

    virtual ~Bridge() {}

    virtual void init();

private slots:
    void defaultAdapterChanged(const QDBusObjectPath &);
    void addDevice(const QDBusObjectPath &v);
    void removeDevice(const QDBusObjectPath &v);

private:

    QDBusConnection &bus_;
    QDBusObjectPath defaultAdapter_;
    std::unique_ptr<Manager> manager_;
    std::unique_ptr<Adapter> adapter_;
    std::map<QString, std::shared_ptr<Device> > devices_;
    std::set<QString> connected_;
    statefs::qt::ServiceWatch watch_;
};

class BlueZ : public statefs::qt::Namespace
{
public:

    BlueZ(QDBusConnection &bus);

private:
    friend class Bridge;
    void reset_properties();
    statefs::qt::DefaultProperties defaults_;
};

}}

#endif // _STATEFS_PRIVATE_BLUEZ_HPP_
