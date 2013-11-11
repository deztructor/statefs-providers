/*
 * StateFS BlueZ provider
 *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Denis Zalevskiy <denis.zalevskiy@jollamobile.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include "provider_bluez.hpp"
#include <iostream>
#include <functional>
#include <cor/util.hpp>
#include <statefs/qt/dbus.hpp>
#include "dbus_types.hpp"

namespace statefs { namespace bluez {

using statefs::qt::Namespace;
using statefs::qt::PropertiesSource;
using statefs::qt::sync;
using statefs::qt::async;

static char const *service_name = "org.bluez";

Bridge::Bridge(BlueZ *ns, QDBusConnection &bus)
    : PropertiesSource(ns)
    , bus_(bus)
    , watch_(bus, service_name)
{
}

void Bridge::init()
{
    auto setup_manager = [this]() {
        qDebug() << "Setup bluez manager";
        manager_.reset(new Manager(service_name, "/", bus_));
        connect(manager_.get(), &Manager::DefaultAdapterChanged
                , this, &Bridge::defaultAdapterChanged);

        using namespace std::placeholders;
        async(this, manager_->DefaultAdapter()
              , std::bind(&Bridge::defaultAdapterChanged, this,_1));
    };
    auto reset_manager = [this]() {
        qDebug() << "Reset bluez manager";
        manager_.reset();
        static_cast<BlueZ*>(target_)->reset_properties();
    };
    watch_.init(setup_manager, reset_manager);
    setup_manager();
}

void Bridge::defaultAdapterChanged(const QDBusObjectPath &v)
{
    qDebug() << "New default bluetooth adapter" << v.path();
    defaultAdapter_ = v;

    adapter_.reset(new Adapter(service_name, v.path(), bus_));

    sync(adapter_->GetProperties()
         , [this](QVariantMap const &v) {
             setProperties(v);
         });

    connect(adapter_.get(), &Adapter::PropertyChanged
            , [this](const QString &name, const QDBusVariant &value) {
                updateProperty(name, value.variant());
            });

    async(adapter_->ListDevices()
         , [this](const QList<QDBusObjectPath> &devs) {
             foreach(QDBusObjectPath dev, devs)
                 addDevice(dev);
         });

    connect(adapter_.get(), &Adapter::DeviceRemoved
            , this, &Bind::removeDevice);
    connect(adapter_.get(), &Adapter::DeviceCreated
            , this, &Bind::addDevice);
}

static const QString headset_uuid = "00001108";
static bool isHeadset(QString const &uuid)
{
    auto id = uuid.left(headset_uuid.size());
    return id == headset_uuid;
}

void Bridge::addDevice(const QDBusObjectPath &v)
{
    removeDevice(v);
    auto path = v.path();
    auto device = std::make_shared<Device>(service_name, path, bus_);

    auto check_headset = [this, path](bool is_connected, bool is_headset) {
        if (!is_headset)
            return;

        qDebug() << "Headset " << path << " is " << (!is_connected ? "dis" : "") << "connected";
        if (is_connected) {
            connected_headset_ = path;
            updateProperty("Headset", is_connected);
        } else {
            if (path == connected_headset_) {
                updateProperty("Headset", is_connected);
                connected_headset_ = "";
            }
        }
    };

    auto on_connected = [this, path, check_headset](bool is_connected) {
        if (is_connected)
            connected_.insert(path);
        else
            connected_.erase(path);
        updateProperty("Connected", connected_.size() > 0);
        check_headset(is_connected, headsets_.count(path));
    };

    auto on_uuid = [this, path, check_headset](QStringList const &uuids) {
        auto is_headset = std::any_of(uuids.begin(), uuids.end(), isHeadset);
        if (is_headset)
            headsets_.insert(path);
        else
            headsets_.erase(path);

        check_headset(connected_.count(path), is_headset);
    };

    auto processProperty = [on_connected, on_uuid]
        (const QString &name, const QVariant &value) {
        if (name == QLatin1String("Connected")) {
            on_connected(value.toBool());
        } else if (name == QLatin1String("UUIDs")) {
            on_uuid(value.toStringList());
        }
    };

    devices_.insert(std::make_pair(path, device));
    async(device->GetProperties()
          , [processProperty](const QVariantMap &props) {
              for (auto it = props.begin(); it != props.end(); ++it)
                  processProperty(it.key(), it.value());
          });
    connect(device.get(), &Device::PropertyChanged
            , [processProperty](const QString &name, const QDBusVariant &value) {
                processProperty(name, value.variant());
            });
}

void Bridge::removeDevice(const QDBusObjectPath &v)
{
    auto path = v.path();
    devices_.erase(path);
    if (connected_.erase(path))
        updateProperty("Connected", connected_.size() > 0);

    if (headsets_.erase(path) && path == connected_headset_) {
        updateProperty("Headset", false);
        connected_headset_ = "";
    }
}

BlueZ::BlueZ(QDBusConnection &bus)
    : Namespace("Bluetooth", std::unique_ptr<PropertiesSource>
                (new Bridge(this, bus)))
    , defaults_({
            { "Enabled", "0" }
            , { "Visible", "0" }
            , { "Connected", "0" }
            , { "Address", "00:00:00:00:00:00" }
            , { "Headset", "0" }})
{
    addProperty("Enabled", "0", "Powered");
    addProperty("Visible", "0", "Discoverable");
    addProperty("Connected", "0");
    addProperty("Address", "00:00:00:00:00:00");
    addProperty("Headset", "0");
    src_->init();
}

void BlueZ::reset_properties()
{
    setProperties(defaults_);
}


class Provider : public statefs::AProvider
{
public:
    Provider(statefs_server *server)
        : AProvider("bluez", server)
        , bus_(QDBusConnection::systemBus())
    {
        auto ns = std::make_shared<BlueZ>(bus_);
        insert(std::static_pointer_cast<statefs::ANode>(ns));
    }
    virtual ~Provider() {}

    virtual void release() {
        delete this;
    }

private:
    QDBusConnection bus_;
};

static Provider *provider = nullptr;

static inline Provider *init_provider(statefs_server *server)
{
    if (provider)
        throw std::logic_error("provider ptr is already set");
    registerDataTypes();
    provider = new Provider(server);
    return provider;
}

}}

EXTERN_C struct statefs_provider * statefs_provider_get
(struct statefs_server *server)
{
    return statefs::bluez::init_provider(server);
}
