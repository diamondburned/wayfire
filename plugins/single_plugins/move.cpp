#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/view.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/compositor-view.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/touch/touch.hpp>
#include <wayfire/plugins/vswitch.hpp>

#include <cmath>
#include <linux/input.h>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/plugins/common/preview-indication.hpp>

#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/common/move-drag-interface.hpp>


#include "snap_signal.hpp"
#include <wayfire/plugins/common/view-change-viewport-signal.hpp>

class wayfire_move : public wf::plugin_interface_t
{
    wf::signal_callback_t move_request, view_destroyed;
    wf::button_callback activate_binding;

    wf::option_wrapper_t<bool> enable_snap{"move/enable_snap"};
    wf::option_wrapper_t<bool> join_views{"move/join_views"};
    wf::option_wrapper_t<int> snap_threshold{"move/snap_threshold"};
    wf::option_wrapper_t<int> quarter_snap_threshold{"move/quarter_snap_threshold"};
    wf::option_wrapper_t<int> workspace_switch_after{"move/workspace_switch_after"};
    wf::option_wrapper_t<wf::buttonbinding_t> activate_button{"move/activate"};

    wf::option_wrapper_t<bool> move_enable_snap_off{"move/enable_snap_off"};
    wf::option_wrapper_t<int> move_snap_off_threshold{"move/snap_off_threshold"};

    bool is_using_touch;
    bool was_client_request;

    struct
    {
        nonstd::observer_ptr<wf::preview_indication_view_t> preview;
        int slot_id = 0;
    } slot;


    wf::wl_timer workspace_switch_timer;

    wf::shared_data::ref_ptr_t<wf::move_drag::core_drag_t> drag_helper;

    bool can_handle_drag()
    {
        bool yes = output->can_activate_plugin(grab_interface,
            wf::PLUGIN_ACTIVATE_ALLOW_MULTIPLE);
        return yes;
    }

    wf::signal_connection_t on_drag_output_focus = [=] (auto data)
    {
        auto ev = static_cast<wf::move_drag::drag_focus_output_signal*>(data);
        if ((ev->focus_output == output) && can_handle_drag())
        {
            drag_helper->set_scale(1.0);

            if (!output->is_plugin_active(grab_interface->name))
            {
                grab_input(nullptr);
            }
        }
    };

    wf::signal_connection_t on_drag_snap_off = [=] (auto data)
    {
        auto ev = static_cast<wf::move_drag::snap_off_signal*>(data);
        if ((ev->focus_output == output) && can_handle_drag())
        {
            wf::move_drag::adjust_view_on_snap_off(drag_helper->view);
        }
    };

    wf::signal_connection_t on_drag_done = [=] (auto data)
    {
        auto ev = static_cast<wf::move_drag::drag_done_signal*>(data);
        if ((ev->focused_output == output) && can_handle_drag())
        {
            wf::move_drag::adjust_view_on_output(ev);
        }

        deactivate();
    };

  public:
    void init() override
    {
        grab_interface->name = "move";
        grab_interface->capabilities =
            wf::CAPABILITY_GRAB_INPUT | wf::CAPABILITY_MANAGE_DESKTOP;

        activate_binding = [=] (auto)
        {
            is_using_touch     = false;
            was_client_request = false;
            auto view = wf::get_core().get_cursor_focus_view();

            if (view && (view->role != wf::VIEW_ROLE_DESKTOP_ENVIRONMENT))
            {
                return initiate(view);
            }

            return false;
        };

        output->add_button(activate_button, &activate_binding);

        using namespace std::placeholders;
        grab_interface->callbacks.pointer.button =
            [=] (uint32_t b, uint32_t state)
        {
            /* the request usually comes with the left button ... */
            if ((state == WLR_BUTTON_RELEASED) && was_client_request &&
                (b == BTN_LEFT))
            {
                return input_pressed(state, false);
            }

            if (b != wf::buttonbinding_t(activate_button).get_button())
            {
                return;
            }

            is_using_touch = false;
            input_pressed(state, false);
        };

        grab_interface->callbacks.pointer.motion = [=] (int x, int y)
        {
            handle_input_motion();
        };

        grab_interface->callbacks.touch.motion =
            [=] (int32_t id, int32_t sx, int32_t sy)
        {
            handle_input_motion();
        };

        grab_interface->callbacks.touch.up = [=] (int32_t id)
        {
            if (wf::get_core().get_touch_state().fingers.empty())
            {
                input_pressed(WLR_BUTTON_RELEASED, false);
            }
        };

        grab_interface->callbacks.cancel = [=] ()
        {
            input_pressed(WLR_BUTTON_RELEASED, false);
        };

        move_request =
            std::bind(std::mem_fn(&wayfire_move::move_requested), this, _1);
        output->connect_signal("view-move-request", &move_request);

        drag_helper->connect_signal("focus-output", &on_drag_output_focus);
        drag_helper->connect_signal("snap-off", &on_drag_snap_off);
        drag_helper->connect_signal("done", &on_drag_done);

        // view_destroyed = [=] (wf::signal_data_t *data)
        // {
        // if (get_signaled_view(data) == view)
        // {
        // input_pressed(WLR_BUTTON_RELEASED, true);
        // }
        // };
        // output->connect_signal("view-disappeared", &view_destroyed);
        // output->connect_signal("view-move-check", &on_view_check_move);
    }

    void move_requested(wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        if (!view)
        {
            return;
        }

        was_client_request = true;
        initiate(view);
    }

    /**
     * Calculate plugin activation flags for the view.
     *
     * Activation flags ignore input inhibitors if the view is in the desktop
     * widget layer (i.e OSKs)
     */
    uint32_t get_act_flags(wayfire_view view)
    {
        uint32_t view_layer = output->workspace->get_view_layer(view);
        /* Allow moving an on-screen keyboard while screen is locked */
        bool ignore_inhibit = view_layer == wf::LAYER_DESKTOP_WIDGET;
        uint32_t act_flags  = 0;
        if (ignore_inhibit)
        {
            act_flags |= wf::PLUGIN_ACTIVATION_IGNORE_INHIBIT;
        }

        return act_flags;
    }

    /**
     * Calculate the view which is the actual target of this move operation.
     *
     * Usually, this is the view itself or its topmost parent if the join_views
     * option is set.
     */
    wayfire_view get_target_view(wayfire_view view)
    {
        while (view && view->parent && join_views)
        {
            view = view->parent;
        }

        return view;
    }

    bool can_move_view(wayfire_view view)
    {
        if (!view || !view->is_mapped())
        {
            return false;
        }

        view = get_target_view(view);

        auto current_ws_impl =
            output->workspace->get_workspace_implementation();
        if (!current_ws_impl->view_movable(view))
        {
            return false;
        }

        return output->can_activate_plugin(grab_interface, get_act_flags(view));
    }

    bool grab_input(wayfire_view view)
    {
        view = view ?: drag_helper->view;
        if (!view)
        {
            return false;
        }

        if (!output->activate_plugin(grab_interface, get_act_flags(view)))
        {
            return false;
        }

        if (!grab_interface->grab())
        {
            output->deactivate_plugin(grab_interface);

            return false;
        }

        auto touch = wf::get_core().get_touch_state();
        is_using_touch = !touch.fingers.empty();

        slot.slot_id = 0;
        return true;
    }

    bool initiate(wayfire_view view)
    {
        view = get_target_view(view);
        if (!can_move_view(view))
        {
            return false;
        }

        if (!grab_input(view))
        {
            return false;
        }

        wf::move_drag::drag_options_t opts;
        opts.enable_snap_off = move_enable_snap_off &&
            (view->fullscreen || view->tiled_edges);
        opts.snap_off_threshold = move_snap_off_threshold;
        opts.join_views = join_views;

        // this->view   = view;
        drag_helper->start_drag(view, get_global_input_coords(), opts);

// ensure_move_helper_at(view, get_input_coords());
//
// output->focus_view(view, true);
// if (enable_snap)
// {
// slot.slot_id = 0;
// }
//
// this->view = view;
// output->render->set_redraw_always();
// update_multi_output();
//
        return true;
    }

    void deactivate()
    {
        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);
        // output->render->set_redraw_always(false);
    }

    void input_pressed(uint32_t state, bool view_destroyed)
    {
        if (state != WLR_BUTTON_RELEASED)
        {
            return;
        }

        drag_helper->handle_input_released();

        // MOVE_HELPER->handle_input_released();

        ///* Delete any mirrors we have left, showing an animation */
        // delete_mirror_views(true);

        ///* Don't do snapping, etc for shell views */
        // if (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT)
        // {
        // view->erase_data<wf::move_snap_helper_t>();
        // this->view = nullptr;
        // return;
        // }

        // if (enable_snap && (slot.slot_id != 0))
        // {
        // snap_signal data;
        // data.view = view;
        // data.slot = (slot_type)slot.slot_id;
        // output->emit_signal("view-snap", &data);

        ///* Update slot, will hide the preview as well */
        // update_slot(0);
        // }

        // view_change_viewport_signal workspace_may_changed;
        // workspace_may_changed.view = this->view;
        // workspace_may_changed.to   = output->workspace->get_current_workspace();
        // workspace_may_changed.old_viewport_invalid = false;
        // output->emit_signal("view-change-viewport", &workspace_may_changed);

        // view->erase_data<wf::move_snap_helper_t>();
        // this->view = nullptr;
    }

    /* Calculate the slot to which the view would be snapped if the input
     * is released at output-local coordinates (x, y) */
    int calc_slot(int x, int y)
    {
        auto g = output->workspace->get_workarea();
        if (!(output->get_relative_geometry() & wf::point_t{x, y}))
        {
            return 0;
        }

        // if (view && (output->workspace->get_view_layer(view) !=
        // wf::LAYER_WORKSPACE))
        // {
        // return 0;
        // }

        int threshold = snap_threshold;

        bool is_left   = x - g.x <= threshold;
        bool is_right  = g.x + g.width - x <= threshold;
        bool is_top    = y - g.y < threshold;
        bool is_bottom = g.x + g.height - y < threshold;

        bool is_far_left   = x - g.x <= quarter_snap_threshold;
        bool is_far_right  = g.x + g.width - x <= quarter_snap_threshold;
        bool is_far_top    = y - g.y < quarter_snap_threshold;
        bool is_far_bottom = g.x + g.height - y < quarter_snap_threshold;

        int slot = 0;
        if ((is_left && is_far_top) || (is_far_left && is_top))
        {
            slot = SLOT_TL;
        } else if ((is_right && is_far_top) || (is_far_right && is_top))
        {
            slot = SLOT_TR;
        } else if ((is_right && is_far_bottom) || (is_far_right && is_bottom))
        {
            slot = SLOT_BR;
        } else if ((is_left && is_far_bottom) || (is_far_left && is_bottom))
        {
            slot = SLOT_BL;
        } else if (is_right)
        {
            slot = SLOT_RIGHT;
        } else if (is_left)
        {
            slot = SLOT_LEFT;
        } else if (is_top)
        {
            // Maximize when dragging to the top
            slot = SLOT_CENTER;
        } else if (is_bottom)
        {
            slot = SLOT_BOTTOM;
        }

        return slot;
    }

    void update_workspace_switch_timeout(int slot_id)
    {
        if ((workspace_switch_after == -1) || (slot_id == 0))
        {
            workspace_switch_timer.disconnect();

            return;
        }

        int dx = 0, dy = 0;
        if (slot_id >= 7)
        {
            dy = -1;
        }

        if (slot_id <= 3)
        {
            dy = 1;
        }

        if (slot_id % 3 == 1)
        {
            dx = -1;
        }

        if (slot_id % 3 == 0)
        {
            dx = 1;
        }

        if ((dx == 0) && (dy == 0))
        {
            workspace_switch_timer.disconnect();

            return;
        }

        wf::point_t cws = output->workspace->get_current_workspace();
        wf::point_t tws = {cws.x + dx, cws.y + dy};
        wf::dimensions_t ws_dim = output->workspace->get_workspace_grid_size();
        wf::geometry_t possible = {
            0, 0, ws_dim.width, ws_dim.height
        };

        /* Outside of workspace grid */
        if (!(possible & tws))
        {
            workspace_switch_timer.disconnect();

            return;
        }

        workspace_switch_timer.set_timeout(workspace_switch_after, [this, tws] ()
        {
            // output->workspace->request_workspace(tws, {this->view});
            return false;
        });
    }

    void update_slot(int new_slot_id)
    {
        /* No changes in the slot, just return */
        if (slot.slot_id == new_slot_id)
        {
            return;
        }

        /* Destroy previous preview */
        if (slot.preview)
        {
            auto input = get_input_coords();
            slot.preview->set_target_geometry(
                {input.x, input.y, 1, 1}, 0, true);
            slot.preview = nullptr;
        }

        slot.slot_id = new_slot_id;

        /* Show a preview overlay */
        if (new_slot_id)
        {
            snap_query_signal query;
            query.slot = (slot_type)new_slot_id;
            query.out_geometry = {0, 0, -1, -1};
            output->emit_signal("query-snap-geometry", &query);

            /* Unknown slot geometry, can't show a preview */
            if ((query.out_geometry.width <= 0) || (query.out_geometry.height <= 0))
            {
                return;
            }

            auto input   = get_input_coords();
            auto preview = new wf::preview_indication_view_t(output,
                {input.x, input.y, 1, 1});

            wf::get_core().add_view(
                std::unique_ptr<wf::view_interface_t>(preview));

            preview->set_output(output);
            preview->set_target_geometry(query.out_geometry, 1);
            slot.preview = nonstd::make_observer(preview);
        }

        update_workspace_switch_timeout(new_slot_id);
    }

    /* Returns the currently used input coordinates in global compositor space */
    wf::point_t get_global_input_coords()
    {
        wf::pointf_t input;
        if (is_using_touch)
        {
            auto center = wf::get_core().get_touch_state().get_center().current;
            input = {center.x, center.y};
        } else
        {
            input = wf::get_core().get_cursor_position();
        }

        return {(int)input.x, (int)input.y};
    }

    /* Returns the currently used input coordinates in output-local space */
    wf::point_t get_input_coords()
    {
        auto og     = output->get_layout_geometry();
        auto coords = get_global_input_coords() - wf::point_t{og.x, og.y};

        /* If the currently moved view is not a toplevel view, but a child
         * view, do not move it outside its outpup */
        // if (view && view->parent)
        // {
        // double x   = coords.x;
        // double y   = coords.y;
        // auto local = output->get_relative_geometry();
        // wlr_box_closest_point(&local, x, y, &x, &y);
        // coords = {(int)x, (int)y};
        // }

        return coords;
    }

    void handle_input_motion()
    {
        drag_helper->handle_motion(get_global_input_coords());

        auto input = get_input_coords();
        // MOVE_HELPER->handle_motion(get_input_coords());

        // update_multi_output();
        /* View might get destroyed when updating multi-output */
        if (false)
        {
            // Make sure that fullscreen views are not tiled.
            // We allow movement of fullscreen views but they should always
            // retain their fullscreen state (but they can be moved to other
            // workspaces). Unsetting the fullscreen state can break some
            // Xwayland games.
            // if (enable_snap && !MOVE_HELPER->is_view_fixed() &&
            // !this->view->fullscreen)
            // {
            // update_slot(calc_slot(input.x, input.y));
            // }
        } else
        {
            /* View was destroyed, hide slot */
            update_slot(0);
        }
    }

    void fini() override
    {
        if (grab_interface->is_grabbed())
        {
            input_pressed(WLR_BUTTON_RELEASED, false);
        }

        output->rem_binding(&activate_binding);
        output->disconnect_signal("view-move-request", &move_request);
        output->disconnect_signal("view-disappeared", &view_destroyed);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_move);
