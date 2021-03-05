#include <vector>
#include "wayfire/debug.hpp"
#include <string>
#include <wayfire/config/file.hpp>
#include <wayfire/config-backend.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>

#include <sys/inotify.h>
#include <unistd.h>

#define INOT_BUF_SIZE (sizeof(inotify_event) + NAME_MAX + 1)

static std::string config_dir, config_file;
wf::config::config_manager_t *cfg_manager;

static void readd_watch(int fd)
{
    inotify_add_watch(fd, config_dir.c_str(), IN_CREATE);
    inotify_add_watch(fd, config_file.c_str(), IN_MODIFY);
}

static void reload_config(int fd)
{
    wf::config::load_configuration_options_from_file(*cfg_manager, config_file);
    readd_watch(fd);
}

static int handle_config_updated(int fd, uint32_t mask, void *data)
{
    LOGD("Reloading configuration file");

    char buf[INOT_BUF_SIZE]
    __attribute__((aligned(alignof(inotify_event))));

    bool should_reload = false;

    while (read(fd, buf, INOT_BUF_SIZE) > 0)
    {
        auto event = reinterpret_cast<inotify_event*>(buf);
        if (event->len == 0)
        {
            // is file, probably the config file itself
            should_reload = true;
            continue;
        }

        if (config_file == event->name)
        {
            should_reload = true;
        }
    }

    if (should_reload)
    {
        reload_config(fd);
        wf::get_core().emit_signal("reload-config", nullptr);
    } else
    {
        readd_watch(fd);
    }

    return 0;
}

static const char *CONFIG_FILE_ENV = "WAYFIRE_CONFIG_FILE";

namespace wf
{
class dynamic_ini_config_t : public wf::config_backend_t
{
  public:
    void init(wl_display *display, config::config_manager_t& config,
        const std::string& cfg_file) override
    {
        cfg_manager = &config;

        config_file = choose_cfg_file(cfg_file);
        LOGI("Using config file: ", config_file.c_str());
        setenv(CONFIG_FILE_ENV, config_file.c_str(), 1);

        config = wf::config::build_configuration(
            get_xml_dirs(), SYSCONFDIR "/wayfire/defaults.ini", config_file);

        int inotify_fd = inotify_init1(IN_CLOEXEC);
        reload_config(inotify_fd);

        wl_event_loop_add_fd(wl_display_get_event_loop(display),
            inotify_fd, WL_EVENT_READABLE, handle_config_updated, NULL);
    }

    std::string choose_cfg_file(const std::string& cmdline_cfg_file)
    {
        std::string env_cfg_file = nonull(getenv(CONFIG_FILE_ENV));
        if (!cmdline_cfg_file.empty())
        {
            if ((env_cfg_file != nonull(NULL)) &&
                (cmdline_cfg_file != env_cfg_file))
            {
                LOGW("Wayfire config file specified in the environment is ",
                    "overridden by the command line arguments!");
            }

            return cmdline_cfg_file;
        }

        if (env_cfg_file != nonull(NULL))
        {
            return env_cfg_file;
        }

        // Fallback, default config file
        config_dir = nonull(getenv("XDG_CONFIG_HOME"));
        if (!config_dir.compare("nil"))
        {
            config_dir = std::string(nonull(getenv("HOME"))) + "/.config";
        }

        return config_dir + "/wayfire.ini";
    }
};
}

DECLARE_WAYFIRE_CONFIG_BACKEND(wf::dynamic_ini_config_t);
