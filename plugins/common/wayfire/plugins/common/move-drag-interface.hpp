#pragma once

#include <wayfire/plugins/wobbly/wobbly-signal.hpp>
#include <wayfire/object.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/nonstd/observer_ptr.h>
#include <wayfire/render-manager.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/util/log.hpp>


namespace wf
{
/**
 * A collection of classes and interfaces which can be used by plugins which
 * support dragging views to move them.
 *
 *  A plugin using these APIs would get support for:
 *
 * - Moving views on the same output, following the pointer or touch position.
 * - Holding views in place until a certain threshold is reached
 * - Wobbly windows (if enabled)
 * - Move the view freely between different outputs with different plugins active
 *   on them, as long as all of these plugins support this interface.
 * - Show smooth transitions of the moving view when moving between different
 *   outputs.
 *
 * A plugin using these APIs is expected to:
 * - Grab input on its respective output and forward any events to the core_drag_t
 *   singleton.
 * - Have activated itself with CAPABILITY_MANAGE_COMPOSITOR
 * - Connect to and handle the signals described below.
 */
namespace move_drag
{
/**
 * name: focus-output
 * on: core_drag_t
 * when: Emitted output whenever the output where the drag happens changes,
 *   including when the drag begins.
 */
struct drag_focus_output_signal : public signal_data_t
{
    /** The output which was focused up to now, might be null. */
    wf::output_t *previous_focus_output;
    /** The output which was focused now. */
    wf::output_t *focus_output;
};

/**
 * name: done
 * on: core_drag_t
 * when: Emitted after the drag operation has ended.
 */
struct drag_done_signal : public signal_data_t
{
    /** The output where the view was dropped. */
    wf::output_t *focused_output;

    /** The view itself. */
    wayfire_view view;

    /**
     * The position relative to the view where the grab was.
     * See scale_around_grab_t::relative_grab
     */
    wf::pointf_t relative_grab;

    /**
     * The position of the input when the view was dropped.
     * In output-layout coordinates.
     */
    wf::point_t grab_position;
};

/**
 * Find the geometry of a view, if it has size @size, it is grabbed at point @grab,
 * and the grab is at position @relative relative to the view.
 */
inline static wf::geometry_t find_geometry_around(
    wf::dimensions_t size, wf::point_t grab, wf::pointf_t relative)
{
    return wf::geometry_t{
        grab.x - (int)std::floor(relative.x * size.width),
        grab.y - (int)std::floor(relative.y * size.height),
        size.width,
        size.height,
    };
}

/**
 * Find the position of grab relative to the view.
 * Example: returns [0.5, 0.5] if the grab is the midpoint of the view.
 */
inline static wf::pointf_t find_relative_grab(
    wf::geometry_t view, wf::point_t grab)
{
    return wf::pointf_t{
        1.0 * (grab.x - view.x) / view.width,
        1.0 * (grab.y - view.y) / view.height,
    };
}

/**
 * A transformer used while dragging.
 *
 * It is primarily used to scale the view is a plugin needs it, and also to keep it
 * centered around the `grab_position`.
 */
class scale_around_grab_t : public wf::view_transformer_t
{
  public:
    /**
     * Factor for scaling down the view.
     * A factor 2.0 means that the view will have half of its width and height.
     */
    wf::animation::simple_animation_t scale_factor{wf::create_option(300)};

    /**
     * A place relative to the view, where it is grabbed.
     *
     * Coordinates are [0, 1]. A grab at (0.5, 0.5) means that the view is grabbed
     * at its center.
     */
    wf::pointf_t relative_grab;

    /**
     * The position where the grab appears on the outputs, in output-layout
     * coordinates.
     */
    wf::point_t grab_position;

    uint32_t get_z_order() override
    {
        return wf::TRANSFORMER_HIGHLEVEL - 1;
    }

    wf::region_t transform_opaque_region(
        wf::geometry_t box, wf::region_t region) override
    {
        // TODO: figure out a way to take opaque region into account
        return {};
    }

    wf::pointf_t scale_around_grab(wf::geometry_t view, wf::pointf_t point,
        double factor)
    {
        auto gx = view.x + view.width * relative_grab.x;
        auto gy = view.y + view.height * relative_grab.y;

        return {
            (point.x - gx) * factor + gx,
            (point.y - gy) * factor + gy,
        };
    }

    wf::pointf_t transform_point(wf::geometry_t view, wf::pointf_t point) override
    {
        LOGE("Unexpected transform_point() call for dragged overlay view!");
        return scale_around_grab(view, point, 1.0 / scale_factor);
    }

    wf::pointf_t untransform_point(wf::geometry_t view, wf::pointf_t point) override
    {
        LOGE("Unexpected untransform_point() call for dragged overlay view!");
        return scale_around_grab(view, point, scale_factor);
    }

    wf::geometry_t get_bounding_box(wf::geometry_t view,
        wf::geometry_t region) override
    {
        int w = std::floor(view.width / scale_factor);
        int h = std::floor(view.height / scale_factor);

        auto bb = find_geometry_around({w, h}, grab_position, relative_grab);
        // LOGI("got bb ", bb);
        return bb;
    }

    void render_with_damage(wf::texture_t src_tex, wlr_box src_box,
        const wf::region_t& damage, const wf::framebuffer_t& target_fb) override
    {
        // Get target size
        auto bbox = get_bounding_box(src_box, src_box);

        OpenGL::render_begin(target_fb);
        for (auto& rect : damage)
        {
            target_fb.logic_scissor(wlr_box_from_pixman_box(rect));
            OpenGL::render_texture(src_tex, target_fb, bbox);
        }

        OpenGL::render_end();
    }
};

static const std::string move_drag_transformer = "move-drag-transformer";

/**
 * An object for storing per-output data.
 */
class output_data_t : public noncopyable_t, public custom_data_t
{
  public:
    output_data_t(wf::output_t *output, wayfire_view view)
    {
        output->render->add_effect(&damage_overlay, OUTPUT_EFFECT_PRE);
        output->render->add_effect(&render_overlay, OUTPUT_EFFECT_OVERLAY);

        this->output = output;
        this->view   = view;
    }

    ~output_data_t()
    {
        output->render->rem_effect(&damage_overlay);
        output->render->rem_effect(&render_overlay);
    }

    void apply_damage()
    {
        // Note: bbox will be in output layout coordinates now, since this is
        // how the transformer works
        auto bbox = view->get_bounding_box();
        bbox = bbox + -wf::origin(output->get_layout_geometry());

        output->render->damage(bbox);
        output->render->damage(last_bbox);

        last_bbox = bbox;
    }

  private:
    wf::output_t *output;
    wayfire_view view;

    /**
     * The last bounding box used for damage.
     * This is needed in case the view resizes or something like that, in which
     * case we don't have access to the previous bbox.
     */
    wf::geometry_t last_bbox = {0, 0, 0, 0};

    // An effect hook for damaging the view on the current output.
    //
    // This is needed on a per-output basis in order to drive the scaling animation
    // forward, if such an animation is running.
    //
    // TODO: We overdo damage, for ex. in the following cases:
    // - Expo does not need any damage (can't really be fixed, since we don't know
    // the plugin which uses this API).
    // - If the view has not updated, and cursor has not moved
    effect_hook_t damage_overlay = [=] ()
    {
        apply_damage();
    };

    effect_hook_t render_overlay = [=] ()
    {
        auto fb = output->render->get_target_framebuffer();
        fb.geometry = output->get_layout_geometry();

        // Convert damage from output-local coordinates (last_bbox) to
        // output-layout coords.
        wf::region_t damage;
        damage |= last_bbox + wf::origin(fb.geometry);

        // Render the full view, always
        // Not very efficient
        view->render_transformed(fb, std::move(damage));
    };
};

struct drag_options_t
{
    double initial_scale = 1.0;
};

/**
 * An object for storing global move drag data (i.e shared between all outputs).
 *
 * Intended for use via wf::shared_data::ref_ptr_t.
 */
class core_drag_t : public signal_provider_t
{
    /**
     * Rebuild the wobbly model after a change in the scaling, so that the wobbly
     * model does not try to animate the scaling change itself.
     */
    void rebuild_wobbly(wayfire_view view, wf::point_t grab, wf::pointf_t relative)
    {
        auto dim = wf::dimensions(view->get_bounding_box("wobbly"));
        modify_wobbly(view, find_geometry_around(dim, grab, relative));
    }

  public:
    /**
     * Start drag.
     *
     * @param view The view which is being dragged.
     * @param grab_position The position of the input, in output-layout coordinates.
     * @param relative The position of the grab_position relative to view.
     */
    void start_drag(wayfire_view view, wf::point_t grab_position,
        wf::pointf_t relative,
        const drag_options_t& options)
    {
        this->view = view;

        // Setup view transform
        auto tr = std::make_unique<scale_around_grab_t>();
        this->transformer = {tr};

// auto output_offset = wf::origin(view->get_output()->get_layout_geometry());
        tr->relative_grab = relative;
        tr->grab_position = grab_position;
        tr->scale_factor.animate(options.initial_scale, options.initial_scale);

        view->add_transformer(std::move(tr), move_drag_transformer);

        // Hide the view, we will render it as an overlay
        view->set_visible(false);
        view->damage();

        // Make sure that wobbly has the correct geometry from the start!
        rebuild_wobbly(view, grab_position, relative);

        // TODO: make this configurable!
        start_wobbly_rel(view, relative);

        // Setup overlay hooks
        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            output->store_data(
                std::make_unique<output_data_t>(output, view));
        }

        wf::get_core().set_cursor("grabbing");
    }

    void start_drag(wayfire_view view, wf::point_t grab_position,
        const drag_options_t& options)
    {
        auto bbox = view->get_bounding_box();
        start_drag(view, grab_position,
            find_relative_grab(bbox, grab_position), options);
    }

    void handle_motion(wf::point_t to)
    {
        transformer->grab_position = to;
        move_wobbly(view, to.x, to.y);
        update_current_output(to);
    }

    void handle_input_released()
    {
        // Store data for the drag done signal
        drag_done_signal data;
        data.grab_position = transformer->grab_position;
        data.relative_grab = transformer->relative_grab;
        data.view = view;
        data.focused_output = current_output;

        // Remove overlay hooks and damage outputs BEFORE popping the transformer
        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            output->get_data<output_data_t>()->apply_damage();
            output->erase_data<output_data_t>();
        }

        // Restore view to where it was before
        view->set_visible(true);
        view->pop_transformer(move_drag_transformer);

        // Reset wobbly and leave it in output-LOCAL coordinates
        end_wobbly(view);

        // Important! If the view scale was not 1.0, the wobbly model needs to be
        // updated with the new size. Since this is an artificial resize, we need
        // to make sure that the resize happens smoothly.
        rebuild_wobbly(view, data.grab_position, data.relative_grab);

        // Put wobbly back in output-local space, the plugins will take it from
        // here.
        translate_wobbly(view,
            -wf::origin(view->get_output()->get_layout_geometry()));

        // Reset our state
        view = nullptr;
        current_output = nullptr;

        // Lastly, let the plugins handle what happens on drag end.
        emit_signal("done", &data);
    }

    void set_scale(double new_scale)
    {
        transformer->scale_factor.animate(new_scale);
    }

    // View currently being moved.
    wayfire_view view;

    // Output where the action is happening.
    wf::output_t *current_output = NULL;

  private:
    nonstd::observer_ptr<scale_around_grab_t> transformer;
    void update_current_output(wf::point_t grab)
    {
        wf::pointf_t origin = {1.0 * grab.x, 1.0 * grab.y};
        auto output =
            wf::get_core().output_layout->get_output_coords_at(origin, origin);

        if (output != current_output)
        {
            drag_focus_output_signal data;
            data.previous_focus_output = current_output;

            current_output    = output;
            data.focus_output = output;
            wf::get_core().focus_output(output);
            emit_signal("focus-output", &data);
        }
    }
};

/**
 * Move the view to the target output and put it at the coordinates of the grab.
 */
inline void adjust_view_on_output(drag_done_signal *ev)
{
    if (ev->view->get_output() != ev->focused_output)
    {
        wf::get_core().move_view_to_output(ev->view, ev->focused_output, false);
    }

    auto bbox = ev->view->get_bounding_box("wobbly");
    auto wm   = ev->view->get_wm_geometry();

    wf::point_t wm_offset = wf::origin(wm) + -wf::origin(bbox);
    auto output_delta     = -wf::origin(ev->focused_output->get_layout_geometry());

    auto grab = ev->grab_position + output_delta;
    bbox = wf::move_drag::find_geometry_around(
        wf::dimensions(bbox), grab, ev->relative_grab);

    wf::point_t target = wf::origin(bbox) + wm_offset;

    // Important: wobbly will attempt to translate the view when changing geometry.
    // That's why we need to make sure to "counter" the effect here.
    // translate_wobbly(ev->view, wf::origin(wm) - target);

    ev->view->move(target.x, target.y);
}
}
}
