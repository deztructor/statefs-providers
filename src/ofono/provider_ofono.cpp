/*
 * StateFS oFono provider
 *
 * Properties translation from ofono values is performed in
 * contextkit-compatible way.
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

#include "provider_ofono.hpp"
#include <math.h>
#include <iostream>
#include <statefs/qt/dbus.hpp>
#include "dbus_types.hpp"

#ifdef DEBUG
#define DBG qDebug
#else
struct null_stream {};
template <typename T>
null_stream operator << (null_stream dst, T)
{
    return dst;
}
#define DBG null_stream
#endif

namespace statefs { namespace ofono {

using statefs::qt::Namespace;
using statefs::qt::PropertiesSource;
using statefs::qt::sync;

static char const *service_name = "org.ofono";


template <typename K, typename F, typename ... Args>
void map_exec(std::map<K, F> const &fns, K const &k, Args&&... args)
{
    auto pfn = fns.find(k);
    if (pfn != fns.end())
        pfn->second(std::forward<Args>(args)...);
}

template <typename K, typename F, typename AltFn, typename ... Args>
void map_exec_or(std::map<K, F> const &fns, AltFn alt_fn, K const &k, Args&&... args)
{
    auto pfn = fns.find(k);
    if (pfn != fns.end())
        pfn->second(std::forward<Args>(args)...);
    else
        alt_fn(k, std::forward<Args>(args)...);
}

template <typename T, typename K, typename F, typename ... Args>
void map_member_exec(T *self, std::map<K, F> const &fns, K const &k, Args&&... args)
{
    auto pfn = fns.find(k);
    if (pfn != fns.end()) {
        auto fn = pfn->second;
        (self->*fn)(std::forward<Args>(args)...);
    }
}

template <typename FnT>
bool find_process_object(PathPropertiesArray const &src, FnT fn)
{
    for (auto it = src.begin(); it != src.end(); ++it) {
        auto const &info = *it;
        auto path = std::get<0>(info).path();
        auto props = std::get<1>(info);

        if (fn(path, props))
            return true;
    }
    return false;
}

typedef Bridge::Status Status;

typedef std::map<QString, std::pair<char const *, char const *> > tech_map_type;
typedef std::map<QString, Status> status_map_type;
typedef std::array<QString, size_t(Status::EOE)> status_array_type;


static const tech_map_type tech_map_ = {
    {"gsm", {"gsm", "gprs"}}
    , {"edge", {"gsm", "egprs"}}
    , {"hspa", {"umts", "hspa"}}
    , {"umts", {"umts", "umts"}}
    , {"lte", {"lte", "lte"}}
};

static const status_map_type status_map_ = {
    {"unregistered", Status::Offline}
    , {"registered", Status::Registered}
    , {"searching", Status::Searching}
    , {"denied", Status::Denied}
    , {"unknown", Status::Unknown}
    , {"roaming", Status::Roaming}
};

static const status_array_type ckit_status_ = {{
    "no-sim", "offline", "home"
    , "offline", "forbidden", "offline", "roam"
    }};

static const status_array_type ofono_status_ = {{
    "unregistered", "unregistered", "registered"
    , "searching", "denied", "unknown", "roaming"
    }};

static const std::array<bool, size_t(Status::EOE)> status_registered_ = {{
    false, false, true, false, false, false, true
    }};

static const std::map<QString, QString> sim_props_map_ = {
    { "MobileCountryCode", "HomeMCC" }
    , { "MobileNetworkCode", "HomeMNC" }
};

static Bridge::property_action_type direct_update(QString const &name)
{
    return [name](Bridge *self, QVariant const &v) {
        self->updateProperty(name, v);
    };
}

static Bridge::property_action_type bind_member
(void (Bridge::*fn)(QVariant const&))
{
    using namespace std::placeholders;
    return [fn](Bridge *self, QVariant const &v) {
        (self->*fn)(v);
    };
}

const Bridge::property_map_type Bridge::net_property_actions_ = {
    { "Name", bind_member(&Bridge::set_network_name)}
    , { "Strength", [](Bridge *self, QVariant const &v) {
            auto strength = v.toUInt();
            self->updateProperty("SignalStrength", strength);
            // 0-5
            self->updateProperty("SignalBars", (strength + 19) / 20);
        } }
    , { "Status", [](Bridge *self, QVariant const &v) {
            self->updateProperty("Status", v);
            self->set_status(self->map_status(v.toString()));
        } }
    , { "CellId", direct_update("CellName") }
    , { "MobileCountryCode", direct_update("CurrentMCC") }
    , { "MobileNetworkCode", direct_update("CurrentMNC") }
    , { "CellId", direct_update("CellName") }
    , { "Technology", [](Bridge *self, QVariant const &v) {
            auto tech = v.toString();
            auto pt = tech_map_.find(tech);
            if (pt != tech_map_.end()) {
                auto tech_dtech = pt->second;
                self->updateProperty("Technology", tech_dtech.first);
                self->updateProperty("DataTechnology", tech_dtech.second);
            }
        } }
};

const Bridge::property_map_type Bridge::operator_property_actions_ = {
    { "Name", bind_member(&Bridge::set_operator_name)}
};


Bridge::Bridge(MainNs *ns, QDBusConnection &bus)
    : PropertiesSource(ns)
    , bus_(bus)
    , watch_(bus, service_name)
    , has_sim_(false)
    , status_(Status::Unknown)
    , network_name_{"", ""}
    , set_name_(&Bridge::set_name_home)
{
}

void Bridge::set_network_name(QVariant const &v)
{
    network_name_.first = v.toString();
    (this->*set_name_)();
}

void Bridge::set_operator_name(QVariant const &v)
{
    network_name_.second = v.toString();
    (this->*set_name_)();
}

void Bridge::set_name_home()
{
    auto name = network_name_.first;
    if (!name.size())
        name = network_name_.second;
    updateProperty("NetworkName", name);
    updateProperty("ExtendedNetworkName", name);
}

void Bridge::set_name_roaming()
{
    auto name = network_name_.first;
    if (!name.size())
        name = network_name_.second;
    updateProperty("NetworkName", name);
    updateProperty("ExtendedNetworkName", name);
}

Status Bridge::map_status(QString const &name)
{
    auto it = status_map_.find(name);
    return (it != status_map_.end()) ? it->second : Status::Offline;
}

void Bridge::set_status(Status new_status)
{
    if (new_status == status_)
        return;

    DBG() << "Set Status " << (int)status_ << "->" << (int)new_status;
    if (!has_sim_ && new_status != Status::NoSim) {
        qWarning() << "No sim, should network properties be processed?";
        set_status(Status::NoSim);
        return;
    }
    if (new_status == Status::NoSim)
        has_sim_ = false;

    auto expected = (new_status == Status::Roaming
                     ? &Bridge::set_name_roaming
                     : &Bridge::set_name_home);
    if (expected != set_name_) {
        set_name_ = expected;

    }

    auto iwas = static_cast<size_t>(status_);
    auto inew = static_cast<size_t>(new_status);
    auto is_registered = status_registered_[inew];

    if (is_registered != status_registered_[iwas]) {
        qDebug() << (is_registered ? "Registered" : "Unregistered");
        if (is_registered) {
            if (!modem_) {
                qWarning() << "Network w/o modem?";
            } else if (!sim_) {
                setup_sim(modem_path_);
            } else if (!network_) {
                setup_network(modem_path_);
            } else {
                enumerate_operators();
            }
        }
    }
    status_ = new_status;
    
    updateProperty("RegistrationStatus", ckit_status_[inew]);
}

void Bridge::reset_modem()
{
    qDebug() << "Reset mode properties";
    modem_path_ = "";
    modem_.reset();
    reset_sim();
}

void Bridge::reset_sim()
{
    qDebug() << "Reset sim properties";
    operator_.reset();
    network_.reset();
    sim_.reset();
    set_status(Status::NoSim);
    static_cast<MainNs*>(target_)->resetProperties(MainNs::NoSimDefault);
}

void Bridge::reset_network()
{
    qDebug() << "Reset network properties";
    operator_.reset();
    network_.reset();
    set_status(Status::Offline);
    auto prop_set = has_sim_ ? MainNs::Default : MainNs::NoSimDefault;
    static_cast<MainNs*>(target_)->resetProperties(prop_set);
}

void Bridge::process_features(QStringList const &v)
{
    auto features = QSet<QString>::fromList(v);
    qDebug() << "Features: " << features;
    bool has_sim_feature = features.contains("sim");
    if (!has_sim_feature) {
        qDebug() << "No sim feature";
        reset_sim();
    } else if (has_sim_feature) {
        setup_sim(modem_path_);
    }
    bool has_net = features.contains("net");
    if (network_) {
        if (has_sim_feature && !has_net)
            reset_network();
    } else if (has_net && has_sim_) {
        setup_network(modem_path_);
    }
}

bool Bridge::setup_modem(QString const &path, QVariantMap const &props)
{
    if (props["Type"].toString() != "hardware") {
        // TODO hardcoded for phones now, no support for e.g. DUN
        return false;
    }
    qDebug() << "Hardware modem " << path;

    auto update = [this](QString const &n, QVariant const &v) {
        DBG() << "Modem prop: " << n << "=" << v;
        if (n == "Features")
            process_features(v.toStringList());
        else if (n == "Powered") {
            if (!v.toBool() && !sim_)
                reset_sim();
        }
    };

    modem_.reset(new Modem(service_name, path, bus_));
    modem_path_ = path;

    connect(modem_.get(), &Modem::PropertyChanged
            , [update](QString const &n, QDBusVariant const &v) {
                update(n, v.variant());
            });
    for (auto it = props.begin(); it != props.end(); ++it)
        update(it.key(), it.value());
    return true;
}

bool Bridge::setup_operator(QString const &path, QVariantMap const &props)
{
    qDebug() << "Operator " << props["Name"];
    auto status = props["Status"].toString();
    if (status != "current")
        return false;

    qDebug() << "Setup current operator properties";

    auto update = [this](QString const &n, QVariant const &v) {
        DBG() << "Operator prop: " << n << "=" << v;
        if (!sim_) {
            DBG() << "No sim, reset network";
            reset_network();
        }
        map_exec(operator_property_actions_, n, this, v);
    };

    operator_.reset(new Operator(service_name, path, bus_));
    operator_path_ = path;
    connect(operator_.get(), &Operator::PropertyChanged
            , [update](QString const &n, QDBusVariant const &v) {
                update(n, v.variant());
            });
    for (auto it = props.begin(); it != props.end(); ++it)
        update(it.key(), it.value());
    return true;
}

void Bridge::init()
{
    qDebug() << "Establish connection with ofono";

    auto connect_manager = [this]() {
        manager_.reset(new Manager(service_name, "/", bus_));
        auto res = sync(manager_->GetModems());
        if (res.isError()) {
            qWarning() << "GetModems error:" << res.error();
            return;
        }
        auto modems = res.value();
        qDebug() << "There is(are) " << modems.size() << " modems";
        if (!modems.size())
            return;

        using namespace std::placeholders;
        find_process_object(modems, std::bind(&Bridge::setup_modem, this, _1, _2));
        connect(manager_.get(), &Manager::ModemAdded
                , [this](QDBusObjectPath const &n, QVariantMap const&p) {
                    setup_modem(n.path(), p);
                });
        connect(manager_.get(), &Manager::ModemRemoved
                , [this](QDBusObjectPath const &n) {
                    if (n.path() == modem_path_)
                        reset_modem();
                });
    };
    auto reset_manager = [this]() {
        qDebug() << "Ofono is unregistered, cleaning up";
        manager_.reset();
        static_cast<MainNs*>(target_)->resetProperties(MainNs::Default);
    };
    watch_.init(connect_manager, reset_manager);
    connect_manager();
}

void Bridge::setup_network(QString const &path)
{
    qDebug() << "Get network properties";
    auto update = [this](QString const &n, QVariant const &v) {
        DBG() << "Network: prop" << n << "=" << v;
        if (!has_sim_) {
            DBG() << "No sim, reset network";
            reset_network();
        }
        map_exec(net_property_actions_, n, this, v);
    };

    network_.reset(new Network(service_name, path, bus_));
    auto res = sync(network_->GetProperties());
    if (res.isError()) {
        qWarning() << "Network GetProperties error:" << res.error();
        return;
    }

    auto props = res.value();
    for (auto it = props.begin(); it != props.end(); ++it)
        update(it.key(), it.value());

    if (!network_) {
        qDebug() << "No network interface";
        return;
    }
    DBG() << "Connect Network::PropertyChanged";
    connect(network_.get(), &Network::PropertyChanged
            , [update](QString const &n, QDBusVariant const &v) {
                update(n, v.variant());
            });
}

void Bridge::enumerate_operators()
{
    if (!network_) {
        qWarning() << "Can't enumerate operators, network is null";
        return;
    }
    auto ops = sync(network_->GetOperators());
    if (ops.isError()) {
        qWarning() << "Network GetOperators error:" << ops.error();
        return;
    }
    using namespace std::placeholders;
    auto process = std::bind(&Bridge::setup_operator, this, _1, _2);
    find_process_object(ops, process);
}

void Bridge::setup_sim(QString const &path)
{
    qDebug() << "Get sim properties";
    auto update = [this](QString const &n, QVariant const &v) {
        DBG() << "Sim prop: " << n << "=" << v;
        if (n == "Present") {
            has_sim_ = v.toBool();
            if (has_sim_) {
                if (!network_)
                    set_status(Status::Offline);
            } else {
                qDebug() << "Ofono: sim is not present";
                set_status(Status::NoSim);
            }
        } else {
            auto it = sim_props_map_.find(n);
            if (it != sim_props_map_.end())
                updateProperty(it->second, v);
        }
    };
    sim_.reset(new SimManager(service_name, path, bus_));
    auto res = sync(sim_->GetProperties());
    if (res.isError()) {
        qWarning() << "Sim GetProperties error:" << res.error();
        return;
    }
    auto props = res.value();
    for (auto it = props.begin(); it != props.end(); ++it)
        update(it.key(), it.value());

    if (sim_)
        connect(sim_.get(), &SimManager::PropertyChanged
                , [update](QString const &n, QDBusVariant const &v) {
                    update(n, v.variant());
                });
}

void MainNs::resetProperties(MainNs::Properties what)
{
    setProperties(defaults_);
    if (what == NoSimDefault)
        updateProperty("RegistrationStatus", "no-sim");
}

// TODO 2 contexkit properties are not supported yet:
// Phone.Call and Phone.Muted
// There is no components using it so the question
// is should they be supported at all

MainNs::MainNs(QDBusConnection &bus)
    : Namespace("Cellular", std::unique_ptr<PropertiesSource>
                (new Bridge(this, bus)))
    , defaults_({
            { "SignalStrength", "0"}
            , { "DataTechnology", "unknown"}
            , { "RegistrationStatus", "offline"} // contextkit
            , { "Status", "unregistered"} // ofono
            , { "Technology", "unknown"}
            , { "SignalBars", "0"}
            , { "CellName", ""}
            , { "NetworkName", ""}
            , { "ExtendedNetworkName", "" }
            , { "CurrentMCC", "0"}
            , { "CurrentMNC", "0"}
            , { "HomeMCC", "0"}
            , { "HomeMNC", "0"}
        })
{
    for (auto v : defaults_)
        addProperty(v.first, v.second);
    src_->init();
}

class Provider;
static Provider *provider = nullptr;

class Provider : public statefs::AProvider
{
public:
    Provider(statefs_server *server)
        : AProvider("ofono", server)
        , bus_(QDBusConnection::systemBus())
    {
        auto ns = std::make_shared<MainNs>(bus_);
        insert(std::static_pointer_cast<statefs::ANode>(ns));
    }
    virtual ~Provider() {}

    virtual void release() {
        if (this == provider) {
            delete provider;
            provider = nullptr;
        }
    }

private:
    QDBusConnection bus_;
};

static inline Provider *init_provider(statefs_server *server)
{
    registerDataTypes();
    if (provider)
        throw std::logic_error("provider ptr is already set");
    provider = new Provider(server);
    return provider;
}

}}

EXTERN_C struct statefs_provider * statefs_provider_get
(struct statefs_server *server)
{
    return statefs::ofono::init_provider(server);
}
