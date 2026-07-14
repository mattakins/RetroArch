/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>

#include <sys/system_properties.h>

#include <formats/image.h>
#include <string/stdstring.h>
#include <compat/strl.h>
#include <retro_timers.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../common/vulkan_common.h"

#include "../../frontend/frontend_driver.h"
#include "../../frontend/drivers/platform_unix.h"
#include "../../verbosity.h"
#include "../../configuration.h"

typedef struct
{
   gfx_ctx_vulkan_data_t vk;
   unsigned width;
   unsigned height;
   int swap_interval;
   bool surface_lost;
   uint64_t present_generation;
   uint64_t wait_logged_generation;
   unsigned wait_logged_reason;
   VkSurfaceTransformFlagBitsKHR presented_transform;
   VkSurfaceTransformFlagBitsKHR pending_transform;
   bool presented_transform_valid;
   bool pending_transform_valid;
} android_ctx_data_vk_t;

enum android_vk_rotation_wait_reason
{
   ANDROID_VK_WAIT_NONE = 0,
   ANDROID_VK_WAIT_WINDOW,
   ANDROID_VK_WAIT_DIMENSIONS,
   ANDROID_VK_WAIT_ORIENTATION,
   ANDROID_VK_WAIT_SURFACE_CAPABILITIES
};

/* FORWARD DECLARATION */
bool android_display_get_metrics(void *data,
	enum display_metric_types type, float *value);
bool android_display_has_focus(void *data);

static void android_gfx_ctx_vk_destroy(void *data)
{
   android_ctx_data_vk_t *and         = (android_ctx_data_vk_t*)data;
   struct android_app *android_app = (struct android_app*)g_android;

   if (!and)
      return;

   vulkan_context_destroy(&and->vk, android_app->window);

   if (and->vk.context.queue_lock)
      slock_free(and->vk.context.queue_lock);

   free(data);
}

static void *android_gfx_ctx_vk_init(void *video_driver)
{
   struct android_app *android_app = (struct android_app*)g_android;
   android_ctx_data_vk_t *and  = (android_ctx_data_vk_t*)calloc(1, sizeof(*and));

   if (!android_app || !and)
      return NULL;

   if (!vulkan_context_init(&and->vk, VULKAN_WSI_ANDROID))
   {
      android_gfx_ctx_vk_destroy(and);
      return NULL;
   }

   slock_lock(android_app->mutex);
   if (!android_app->window)
   {
      slock_unlock(android_app->mutex);
      android_gfx_ctx_vk_destroy(and);
      return NULL;
   }

   slock_unlock(android_app->mutex);
   return and;
}

static void android_gfx_ctx_vk_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   android_ctx_data_vk_t *and  = (android_ctx_data_vk_t*)data;

   *width  = and->width;
   *height = and->height;
}

static bool android_gfx_ctx_vk_orientation_matches(
      int32_t orientation, unsigned width, unsigned height)
{
   switch (orientation)
   {
      case ACONFIGURATION_ORIENTATION_PORT:
         return height > width;
      case ACONFIGURATION_ORIENTATION_LAND:
         return width > height;
      case ACONFIGURATION_ORIENTATION_SQUARE:
         return width == height;
      default:
         return true;
   }
}

static bool android_gfx_ctx_vk_get_window_size(
      struct android_app *android_app, unsigned *width, unsigned *height)
{
   int32_t native_width;
   int32_t native_height;

   if (!android_app->window)
      return false;

   native_width  = ANativeWindow_getWidth(android_app->window);
   native_height = ANativeWindow_getHeight(android_app->window);
   if (native_width <= 0 || native_height <= 0)
      return false;

   *width  = (unsigned)native_width;
   *height = (unsigned)native_height;
   return true;
}

static bool android_gfx_ctx_vk_get_surface_transform(
      android_ctx_data_vk_t *and, VkSurfaceTransformFlagBitsKHR *transform)
{
   VkSurfaceCapabilitiesKHR capabilities;

   if (and->vk.vk_surface == VK_NULL_HANDLE
         || vkGetPhysicalDeviceSurfaceCapabilitiesKHR(and->vk.context.gpu,
               and->vk.vk_surface, &capabilities) != VK_SUCCESS)
      return false;

   *transform = capabilities.currentTransform;
   return true;
}

static void android_gfx_ctx_vk_cancel_generation(
      android_ctx_data_vk_t *and, struct android_app *android_app,
      uint64_t generation)
{
   if (!generation
         || generation > android_app->redraw_cancelled_generation)
      return;

   if (android_app->rotation_generation == generation)
      android_app->rotation_generation = 0;
   android_app->rotation_pending = false;
   if (android_app->swapchain_recreate_generation == generation)
      android_app->swapchain_recreate_generation = 0;
   if (android_app->swapchain_recreated_generation == generation)
      android_app->swapchain_recreated_generation = 0;
   if (android_app->redraw_window_resize_generation
         > android_app->completed_window_resize_generation)
      android_app->completed_window_resize_generation =
            android_app->redraw_window_resize_generation;
   android_app->window_resize_pending_since_usec = 0;
   android_app->content_rect.changed = false;
   if (and->present_generation == generation)
      and->present_generation = 0;
   and->pending_transform_valid = false;
   RARCH_WARN("[Vulkan] Discarded cancelled redraw generation=%llu.\n",
         (unsigned long long)generation);
}

static void android_gfx_ctx_vk_log_rotation_wait(
      android_ctx_data_vk_t *and, uint64_t generation,
      unsigned reason, int32_t orientation,
      unsigned width, unsigned height)
{
   if (and->wait_logged_generation == generation
         && and->wait_logged_reason == reason)
      return;

   and->wait_logged_generation = generation;
   and->wait_logged_reason     = reason;
   RARCH_LOG("[Vulkan] Rotation generation=%llu waiting: reason=%u "
         "orientation=%d window=%ux%u.\n",
         (unsigned long long)generation, reason, orientation, width, height);
}

static void android_gfx_ctx_vk_mark_surface_recreated(
      android_ctx_data_vk_t *and, struct android_app *android_app,
      const char *source)
{
   uint64_t generation;
   VkSurfaceTransformFlagBitsKHR transform;
   bool transform_valid = android_gfx_ctx_vk_get_surface_transform(and,
         &transform);

   slock_lock(android_app->mutex);
   generation = android_app->rotation_generation;
   if (generation > android_app->redraw_completed_generation
         && generation > android_app->redraw_cancelled_generation)
   {
      android_app->swapchain_recreate_generation  = generation;
      android_app->swapchain_recreated_generation = generation;
      and->present_generation                     = generation;
      and->pending_transform                      = transform;
      and->pending_transform_valid                = transform_valid;
      RARCH_LOG("[Vulkan] Rotation generation=%llu rebuilt by %s; "
            "waiting for first present.\n",
            (unsigned long long)generation, source);
   }
   else
   {
      and->present_generation = 0;
      and->presented_transform       = transform;
      and->presented_transform_valid = transform_valid;
      and->pending_transform_valid   = false;
   }
   slock_unlock(android_app->mutex);
}

static void android_gfx_ctx_vk_complete_redraw(
      android_ctx_data_vk_t *and, struct android_app *android_app)
{
   uint64_t generation = and->present_generation;

   if (!generation)
      return;

   slock_lock(android_app->mutex);
   if (android_app->rotation_generation == generation
         && android_app->swapchain_recreated_generation == generation
         && generation > android_app->redraw_cancelled_generation
         && android_app->redraw_completed_generation < generation)
   {
      android_app->redraw_completed_generation      = generation;
      android_app->rotation_generation              = 0;
      android_app->rotation_pending                 = false;
      android_app->swapchain_recreate_generation    = 0;
      android_app->swapchain_recreated_generation   = 0;
      if (android_app->redraw_window_resize_generation
            > android_app->completed_window_resize_generation)
         android_app->completed_window_resize_generation =
               android_app->redraw_window_resize_generation;
      android_app->window_resize_pending_since_usec = 0;
      android_app->content_rect.changed             = false;
      and->present_generation                       = 0;
      and->wait_logged_generation                   = 0;
      and->wait_logged_reason                       = ANDROID_VK_WAIT_NONE;
      RARCH_LOG("[Vulkan] Rotation generation=%llu completed after first "
            "successful present.\n", (unsigned long long)generation);
      if (and->pending_transform_valid)
      {
         and->presented_transform       = and->pending_transform;
         and->presented_transform_valid = true;
         and->pending_transform_valid   = false;
      }
      scond_broadcast(android_app->cond);
   }
   else if (android_app->rotation_generation != generation)
   {
      RARCH_LOG("[Vulkan] Ignoring stale present generation=%llu; "
            "active generation=%llu.\n", (unsigned long long)generation,
            (unsigned long long)android_app->rotation_generation);
      and->present_generation = 0;
   }
   slock_unlock(android_app->mutex);
}

static void android_gfx_ctx_vk_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   struct android_app *android_app      = (struct android_app*)g_android;
   unsigned new_width                   = 0;
   unsigned new_height                  = 0;
   android_ctx_data_vk_t *and           = (android_ctx_data_vk_t*)data;
   uint64_t generation                  = 0;
   uint64_t requested                   = 0;
   bool window_pending                  = false;
   int32_t orientation                  = 0;
   VkSurfaceTransformFlagBitsKHR transform;
   bool transform_valid                 = false;
   bool needs_recreate                  = false;

   *quit                                = false;
   *resize                              = false;

   if (!android_app || !and)
      return;

   slock_lock(android_app->mutex);
   generation = android_app->rotation_generation;
   requested  = android_app->redraw_requested_generation;
   orientation = android_app->config_orientation;
   android_gfx_ctx_vk_cancel_generation(and, android_app, generation);
   generation = android_app->rotation_generation;
   window_pending = android_app->window_resize_generation
         > android_app->completed_window_resize_generation
         || android_app->content_rect.changed;

   if (requested > android_app->redraw_completed_generation
         && requested > android_app->redraw_cancelled_generation
         && generation != requested)
   {
      slock_unlock(android_app->mutex);
      return;
   }

   if (generation <= android_app->redraw_completed_generation
         && window_pending)
   {
      android_app->completed_window_resize_generation =
            android_app->window_resize_generation;
      android_app->window_resize_pending_since_usec = 0;
      and->vk.flags |= VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;
      android_app->content_rect.changed = false;
   }

   if (generation > android_app->redraw_completed_generation
         && generation > android_app->redraw_cancelled_generation)
   {
      if (!android_app->window || and->surface_lost
            || and->vk.vk_surface == VK_NULL_HANDLE)
      {
         android_gfx_ctx_vk_log_rotation_wait(and, generation,
               ANDROID_VK_WAIT_WINDOW, orientation, 0, 0);
         slock_unlock(android_app->mutex);
         return;
      }

      if (!android_gfx_ctx_vk_get_window_size(android_app,
               &new_width, &new_height))
      {
         android_gfx_ctx_vk_log_rotation_wait(and, generation,
               ANDROID_VK_WAIT_DIMENSIONS, orientation, 0, 0);
         slock_unlock(android_app->mutex);
         return;
      }
      android_app->native_window_width  = new_width;
      android_app->native_window_height = new_height;

      if (!android_gfx_ctx_vk_orientation_matches(orientation,
               new_width, new_height))
      {
         android_gfx_ctx_vk_log_rotation_wait(and, generation,
               ANDROID_VK_WAIT_ORIENTATION, orientation,
               new_width, new_height);
         slock_unlock(android_app->mutex);
         return;
      }

      transform_valid = android_gfx_ctx_vk_get_surface_transform(and,
            &transform);
      if (!transform_valid)
      {
         android_gfx_ctx_vk_log_rotation_wait(and, generation,
               ANDROID_VK_WAIT_SURFACE_CAPABILITIES, orientation,
               new_width, new_height);
         slock_unlock(android_app->mutex);
         return;
      }
      needs_recreate  = new_width != and->width || new_height != and->height
            || !and->presented_transform_valid
            || transform != and->presented_transform;

      if (!needs_recreate)
      {
         android_app->swapchain_recreated_generation = generation;
         and->present_generation                     = generation;
         and->pending_transform                      = transform;
         and->pending_transform_valid                = true;
         RARCH_LOG("[Vulkan] Redraw generation=%llu has no window or "
               "surface-transform change; waiting for a normal present.\n",
               (unsigned long long)generation);
         slock_unlock(android_app->mutex);
         return;
      }

      if (android_app->swapchain_recreated_generation == generation
            && and->vk.swapchain != VK_NULL_HANDLE)
      {
         slock_unlock(android_app->mutex);
         return;
      }

      if (android_app->swapchain_recreated_generation == generation)
         android_app->swapchain_recreated_generation = 0;

      if (android_app->swapchain_recreate_generation != generation)
      {
         android_app->swapchain_recreate_generation = generation;
         and->pending_transform                     = transform;
         and->pending_transform_valid               = true;
         RARCH_LOG("[Vulkan] Rotation generation=%llu armed for "
               "swapchain recreation at %ux%u transform=%u.\n",
               (unsigned long long)generation, new_width, new_height,
               transform_valid ? (unsigned)transform : 0);
      }

      *resize = true;
      slock_unlock(android_app->mutex);
   }
   else
   {
      if (android_app->content_rect.changed)
         and->vk.flags |= VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;

      /* Swapchains are recreated in set_resize as a
       * central place, so use that to trigger swapchain reinit. */
      *resize    = (and->vk.flags & VK_DATA_FLAG_NEED_NEW_SWAPCHAIN) != 0;
      new_width  = android_app->content_rect.width;
      new_height = android_app->content_rect.height;
      slock_unlock(android_app->mutex);
   }

   if (new_width != *width || new_height != *height)
   {
      RARCH_LOG("[Vulkan] Resizing (%ux%u) -> (%ux%u).\n",
              *width, *height, new_width, new_height);

      *width  = new_width;
      *height = new_height;
      *resize = true;
   }
}

static bool android_gfx_ctx_vk_set_resize(void *data,
      unsigned width, unsigned height)
{
   android_ctx_data_vk_t        *and  = (android_ctx_data_vk_t*)data;
   struct android_app *android_app    = (struct android_app*)g_android;
   bool rotation_recreate             = false;
   uint64_t generation                = 0;
   int32_t orientation                = 0;
   VkSurfaceTransformFlagBitsKHR transform;
   bool transform_valid               = false;

   if (!and || !android_app)
      return false;

   slock_lock(android_app->mutex);
   generation = android_app->swapchain_recreate_generation;
   android_gfx_ctx_vk_cancel_generation(and, android_app, generation);
   generation = android_app->swapchain_recreate_generation;
   rotation_recreate = generation
         && generation == android_app->rotation_generation
         && generation > android_app->redraw_completed_generation;
   orientation = android_app->config_orientation;

   if (rotation_recreate)
   {
      if (!android_app->window || and->surface_lost
            || and->vk.vk_surface == VK_NULL_HANDLE)
      {
         slock_unlock(android_app->mutex);
         return false;
      }

      if (!android_gfx_ctx_vk_get_window_size(android_app, &width, &height)
            || !android_gfx_ctx_vk_orientation_matches(orientation,
                  width, height))
      {
         slock_unlock(android_app->mutex);
         return false;
      }
   }
   slock_unlock(android_app->mutex);

   if (!width || !height)
      return false;

   and->width                         = width;
   and->height                        = height;
   RARCH_LOG("[Vulkan] Native window size: %ux%u.\n", and->width, and->height);

   if (rotation_recreate)
      and->vk.context.flags          |= VK_CTX_FLAG_INVALID_SWAPCHAIN;

   if (!vulkan_create_swapchain(&and->vk, and->width, and->height, and->swap_interval))
   {
      RARCH_ERR("[Vulkan] Failed to update swapchain.\n");
      return false;
   }

   if (and->vk.flags & VK_DATA_FLAG_CREATED_NEW_SWAPCHAIN)
      vulkan_acquire_next_image(&and->vk);
   and->vk.context.flags             |=  VK_CTX_FLAG_INVALID_SWAPCHAIN;
   and->vk.flags                     &= ~VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;

   if (rotation_recreate)
   {
      transform_valid = android_gfx_ctx_vk_get_surface_transform(and,
            &transform);
      slock_lock(android_app->mutex);
      if (android_app->rotation_generation == generation
            && generation > android_app->redraw_cancelled_generation)
      {
         android_app->swapchain_recreated_generation = generation;
         and->present_generation                     = generation;
         if (transform_valid)
         {
            and->pending_transform       = transform;
            and->pending_transform_valid = true;
         }
         and->wait_logged_generation                 = 0;
         and->wait_logged_reason                     = ANDROID_VK_WAIT_NONE;
         RARCH_LOG("[Vulkan] Rotation generation=%llu swapchain rebuilt; "
               "waiting for first present.\n",
               (unsigned long long)generation);
      }
      else
         android_gfx_ctx_vk_cancel_generation(and, android_app, generation);
      slock_unlock(android_app->mutex);
   }
   else
   {
      slock_lock(android_app->mutex);
      android_app->content_rect.changed = false;
      slock_unlock(android_app->mutex);
   }

   return true;
}

static bool android_gfx_ctx_vk_set_video_mode(void *data,
      unsigned width, unsigned height,
      bool fullscreen)
{
   struct android_app *android_app = (struct android_app*)g_android;
   android_ctx_data_vk_t *and      = (android_ctx_data_vk_t*)data;
   if (!android_gfx_ctx_vk_get_window_size(android_app,
            &and->width, &and->height))
      return false;
   if (!vulkan_surface_create(&and->vk, VULKAN_WSI_ANDROID,
            NULL, android_app->window,
            and->width, and->height, and->swap_interval))
   {
      RARCH_ERR("[Vulkan] Failed to create surface.\n");
      return false;
   }
   and->surface_lost = false;
   android_gfx_ctx_vk_mark_surface_recreated(and, android_app,
         "initial surface creation");
   RARCH_LOG("[Vulkan] Native window size: %ux%u.\n",
         and->width, and->height);
   return true;
}

static bool android_gfx_ctx_vk_create_surface(void *data)
{
   struct android_app *android_app = (struct android_app*)g_android;
   android_ctx_data_vk_t *and      = (android_ctx_data_vk_t*)data;

   /* APP_CMD_INIT_WINDOW can remain pending after startup even though the
    * Vulkan surface is already active. */
   if (and && !and->surface_lost
         && and->vk.vk_surface != VK_NULL_HANDLE)
   {
      RARCH_LOG("[Vulkan] Ignoring duplicate Android window initialization.\n");
      return true;
   }

   if (!android_app || !android_app->window || !and)
      return false;

   if (!android_gfx_ctx_vk_get_window_size(android_app,
            &and->width, &and->height))
      return false;

   if (!vulkan_surface_create(&and->vk, VULKAN_WSI_ANDROID,
            NULL, android_app->window,
            and->width, and->height, and->swap_interval))
   {
      RARCH_ERR("[Vulkan] Failed to recreate Android surface.\n");
      return false;
   }

   and->surface_lost = false;
   android_gfx_ctx_vk_mark_surface_recreated(and, android_app,
         "surface recreation");
   RARCH_LOG("[Vulkan] Recreated Android surface: %ux%u.\n",
         and->width, and->height);
   return true;
}

static bool android_gfx_ctx_vk_destroy_surface(void *data)
{
   android_ctx_data_vk_t *and = (android_ctx_data_vk_t*)data;

   if (!and)
      return false;

   and->surface_lost = true;
   if (!vulkan_surface_destroy(&and->vk))
      return false;

   return true;
}

static void android_gfx_ctx_vk_input_driver(void *data,
      const char *joypad_name,
      input_driver_t **input, void **input_data)
{
   void *androidinput   = input_driver_init_wrap(&input_android, joypad_name);

   *input               = androidinput ? &input_android : NULL;
   *input_data          = androidinput;
}

static enum gfx_ctx_api android_gfx_ctx_vk_get_api(void *data)
{
   return GFX_CTX_VULKAN_API;
}

static bool android_gfx_ctx_vk_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   return (api == GFX_CTX_VULKAN_API);
}


static bool android_gfx_ctx_vk_suppress_screensaver(void *data, bool enable) { return false; }

static void android_gfx_ctx_vk_swap_buffers(void *data)
{
   android_ctx_data_vk_t *and  = (android_ctx_data_vk_t*)data;
   struct android_app *android_app = (struct android_app*)g_android;
   bool presented = false;

   if (!and || !android_app || and->surface_lost
         || and->vk.vk_surface == VK_NULL_HANDLE)
      return;

   if (and->vk.context.flags & VK_CTX_FLAG_HAS_ACQUIRED_SWAPCHAIN)
   {
      and->vk.context.flags &= ~VK_CTX_FLAG_HAS_ACQUIRED_SWAPCHAIN;
      if (and->vk.swapchain == VK_NULL_HANDLE)
      {
         retro_sleep(10);
      }
      else
      {
         presented = vulkan_present(&and->vk,
               and->vk.context.current_swapchain_index);
      }
   }

   if (presented)
      android_gfx_ctx_vk_complete_redraw(and, android_app);

   vulkan_acquire_next_image(&and->vk);
}

static void android_gfx_ctx_vk_set_swap_interval(void *data, int swap_interval)
{
   android_ctx_data_vk_t *and  = (android_ctx_data_vk_t*)data;

   if (and->swap_interval != swap_interval)
   {
      RARCH_LOG("[Vulkan] Setting swap interval: %u.\n", swap_interval);
      and->swap_interval       = swap_interval;
      if (and->vk.swapchain)
         and->vk.flags        |= VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;
   }
}

static gfx_ctx_proc_t android_gfx_ctx_vk_get_proc_address(const char *symbol) { return NULL; }
static void android_gfx_ctx_vk_bind_hw_render(void *data, bool enable) { }

static void *android_gfx_ctx_vk_get_context_data(void *data)
{
   android_ctx_data_vk_t *and = (android_ctx_data_vk_t*)data;
   return &and->vk.context;
}

static uint32_t android_gfx_ctx_vk_get_flags(void *data)
{
   uint32_t flags = 0;

#if defined(HAVE_SLANG) && defined(HAVE_SPIRV_CROSS)
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_SLANG);
#endif

   return flags;
}

static void android_gfx_ctx_vk_set_flags(void *data, uint32_t flags) { }

const gfx_ctx_driver_t gfx_ctx_vk_android = {
   android_gfx_ctx_vk_init,
   android_gfx_ctx_vk_destroy,
   android_gfx_ctx_vk_get_api,
   android_gfx_ctx_vk_bind_api,
   android_gfx_ctx_vk_set_swap_interval,
   android_gfx_ctx_vk_set_video_mode,
   android_gfx_ctx_vk_get_video_size,
   NULL,                                     /* get_refresh_rate */
   NULL,                                     /* get_video_output_size */
   NULL,                                     /* get_video_output_prev */
   NULL,                                     /* get_video_output_next */
   NULL, /* get_metrics - handled by display server */
   NULL,
   NULL,                                     /* update_title */
   android_gfx_ctx_vk_check_window,
   android_gfx_ctx_vk_set_resize,
   android_display_has_focus,
   android_gfx_ctx_vk_suppress_screensaver,
   false,                                    /* has_windowed */
   android_gfx_ctx_vk_swap_buffers,
   android_gfx_ctx_vk_input_driver,
   android_gfx_ctx_vk_get_proc_address,
   NULL,
   NULL,
   NULL,
   "vk_android",
   android_gfx_ctx_vk_get_flags,
   android_gfx_ctx_vk_set_flags,
   android_gfx_ctx_vk_bind_hw_render,
   android_gfx_ctx_vk_get_context_data,
   NULL,                                     /* make_current */
   android_gfx_ctx_vk_create_surface,
   android_gfx_ctx_vk_destroy_surface
};
