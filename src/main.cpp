#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "spawn/process.hpp"
#include "util/charconv.hpp"
#include "util/print.hpp"

namespace {
constexpr auto debug = false;
declare_autoptr(PCM, pcm, pcm_close);

namespace iface {
auto call        = "org.freedesktop.ModemManager1.Call";
auto modem_voice = "org.freedesktop.ModemManager1.Modem.Voice";
} // namespace iface

auto is_mm_state_active(const dbus_uint32_t state) -> bool {
    enum MMCallState {
        MM_CALL_STATE_UNKNOWN = 0, // default state for a new outgoing call.
        MM_CALL_STATE_DIALING,     // outgoing call started. Wait for free channel.
        MM_CALL_STATE_RINGING_OUT, // outgoing call attached to GSM network, waiting for an answer.
        MM_CALL_STATE_RINGING_IN,  // incoming call is waiting for an answer.
        MM_CALL_STATE_ACTIVE,      // call is active between two peers.
        MM_CALL_STATE_HELD,        // held call (by +CHLD AT command).
        MM_CALL_STATE_WAITING,     // waiting call (by +CCWA AT command).
        MM_CALL_STATE_TERMINATED,  // call is terminated.
    };

    switch(state) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_ACTIVE:
        return true;
    default:
        return false;
    }
}

auto run_script(const std::string& script, const char* const action) -> bool {
    const auto argv = std::vector<const char*>{script.data(), action, nullptr};
    auto       proc = process::Process();
    ensure(proc.start({.argv = argv, .die_on_parent_exit = true}));
    ensure(proc.join());
    return true;
}

struct Runtime {
    std::array<process::Process, 2> pw_loopbacks;
    std::array<AutoPCM, 2>          pcms;
};

struct Context {
    uint32_t                 card;
    uint32_t                 device;
    const char*              script;
    std::unique_ptr<Runtime> runtime;
};

auto create_runtime(Context& context) -> std::unique_ptr<Runtime> {
    static const auto pcm_config_voice_call = pcm_config{
        .channels     = 1,
        .rate         = 8000,
        .period_size  = 160,
        .period_count = 2,
        .format       = PCM_FORMAT_S16_LE,
    };

    auto rt = std::unique_ptr<Runtime>(new Runtime());

    // start codecs
    for(auto i = 0; i < 2; i += 1) {
        auto pcm = AutoPCM(pcm_open(context.card, context.device, i == 0 ? PCM_IN : PCM_OUT, &pcm_config_voice_call));
        ensure(pcm_is_ready(pcm.get()));
        ensure(pcm_prepare(pcm.get()) == 0);
        rt->pcms[i] = std::move(pcm);
    }

    // add loopback devices to prevent codec from suspending
    for(auto i = 0; i < 2; i += 1) {
        auto&      proc = rt->pw_loopbacks[i];
        const auto argv = std::vector<const char*>{
            "/bin/pw-loopback",
            i == 0 ? "--capture-props=media.class=Audio/Sink" : "--playback-props=media.class=Audio/Source",
            nullptr,
        };
        ensure(proc.start({.argv = argv, .die_on_parent_exit = true}));
    }

    return rt;
}

auto delete_runtime(std::unique_ptr<Runtime> rt) -> bool {
    for(auto& proc : rt->pw_loopbacks) {
        ensure(proc.join(true));
    }
    return true;
}

auto handle_call_state_changed(Context& context, DBusMessage* const message, DBusError* const error) -> void {
    auto old_state = dbus_uint32_t();
    auto new_state = dbus_uint32_t();
    ensure(dbus_message_get_args(message, error, DBUS_TYPE_INT32, &old_state, DBUS_TYPE_INT32, &new_state, DBUS_TYPE_INVALID) == TRUE);
    if(debug) {
        print("state changed from ", old_state, " to ", new_state);
    }
    if(old_state == new_state) {
        return;
    }
    if(is_mm_state_active(new_state)) {
        if(context.runtime) {
            return;
        }
        if(debug) {
            print("start");
        }
        ensure(run_script(context.script, "voice-start"));
        context.runtime = create_runtime(context);
        ensure(context.runtime);
    } else if(is_mm_state_active(old_state) && !is_mm_state_active(new_state)) {
        if(!context.runtime) {
            return;
        }
        if(debug) {
            print("stop");
        }
        ensure(delete_runtime(std::exchange(context.runtime, {})));
        ensure(run_script(context.script, "voice-stop"));
    }
}

auto handle_message(Context& context, DBusMessage* const message, DBusError* const error) -> void {
    const auto interface = std::string_view(dbus_message_get_interface(message));
    const auto member    = std::string_view(dbus_message_get_member(message));
    if(debug) {
        print("message ", interface, " ", member);
    }
    if(interface == iface::call) {
        if(member == "StateChanged") {
            return handle_call_state_changed(context, message, error);
        }
    } else if(interface == iface::modem_voice) {
        if(member == "CallAdded") {
            ensure(run_script(context.script, "call-added"));
        }
    }
}
} // namespace

auto main(const int argc, const char* const* const argv) -> int {
    if(argc != 4) {
        print("usage: q6voiced CARD_NUM DEVICE_NUM CALLBACK_SCRIPT");
        return 1;
    }
    unwrap(card, from_chars<uint32_t>(argv[1]));
    unwrap(device, from_chars<uint32_t>(argv[2]));
    const auto script = argv[3];

    auto context = Context{.card = card, .device = device, .script = script};

    auto error = DBusError();
    dbus_error_init(&error);
    auto conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    ensure(!dbus_error_is_set(&error), error.message);
    ensure(conn != NULL);
    dbus_bus_add_match(conn, build_string("type='signal',interface='", iface::call, "'").data(), &error);
    dbus_bus_add_match(conn, build_string("type='signal',interface='", iface::modem_voice, "'").data(), &error);
    dbus_connection_flush(conn);
    ensure(!dbus_error_is_set(&error), error.message);

    while(dbus_connection_read_write(conn, -1)) {
        while(auto message = dbus_connection_pop_message(conn)) {
            handle_message(context, message, &error);
            ensure(!dbus_error_is_set(&error), error.message);
            dbus_message_unref(message);
        }
    }

    return 0;
}
