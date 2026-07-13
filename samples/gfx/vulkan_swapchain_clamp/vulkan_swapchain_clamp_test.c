/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (vulkan_swapchain_clamp_test.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Regression test for swapchain image-count bounds in
 * gfx/common/vulkan_common.c::vulkan_create_swapchain.
 *
 * The function calls vkGetSwapchainImagesKHR twice -- once to
 * query the count, once to fill the swapchain_images[] array:
 *
 *   vkGetSwapchainImagesKHR(device, swapchain, &count, NULL);
 *   vkGetSwapchainImagesKHR(device, swapchain, &count, images);
 *
 * Pre-fix, no clamp existed between the two calls.  context.
 * swapchain_images, swapchain_fences, the four
 * swapchain_*_semaphores arrays, readback.staging[] and
 * vk->swapchain[] are all sized at compile time to
 * VULKAN_MAX_SWAPCHAIN_IMAGES (16).  If a driver returned more
 * images than that on the count query, the second call wrote
 * past swapchain_images[16], and every loop bounded by
 * num_swapchain_images (~12 sites across init/deinit/textures/
 * buffers/descriptor pools/command buffers/readback and direct
 * vk->swapchain[i] access) walked past its compile-time-sized
 * array.
 *
 * The Vulkan spec permits the driver to return more images than
 * minImageCount requested (drivers may use triple-buffer-plus
 * strategies, the spec only requires the minimum); on fast
 * MAILBOX drivers under high refresh rates this is observed in
 * practice.
 *
 * Fix: cap desired_swapchain_images to VULKAN_MAX_SWAPCHAIN_IMAGES
 * before vkCreateSwapchainKHR, so well-behaved drivers are never
 * asked for more than we can hold; reject a swapchain whose actual
 * image count exceeds that capacity; and validate every acquired
 * image index before using it to access fixed-size arrays. Merely
 * filling the first 16 images is unsafe because vkAcquireNextImageKHR
 * can still return any image owned by the swapchain.
 *
 * IMPORTANT: this test keeps a verbatim copy of the bounds
 * predicates from vulkan_common.c.  If vulkan_create_swapchain
 * amends them, the copies below must follow.  Convention used
 * by the security regression tests in samples/tasks/.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Match the production constant exactly.  Defined in
 * gfx/common/vulkan_common.h.  If that header changes, this
 * mirror must follow. */
#define VULKAN_MAX_SWAPCHAIN_IMAGES 16

/* Match the production layout of `struct vulkan_context` for the
 * fields the post-fix predicate touches.  We don't model the
 * neighbouring fields literally -- ASan instruments the
 * containing malloc, so an OOB write off swapchain_images[]
 * trips heap-buffer-overflow regardless of layout. */
struct mock_vk_context {
   /* Mock VkImage handles -- pointer-sized opaque.  In production
    * VkImage is a 64-bit dispatchable handle. */
   void    *swapchain_images[VULKAN_MAX_SWAPCHAIN_IMAGES];
   uint32_t num_swapchain_images;
};

/* Mock driver: configurable count returned on first call.
 * In production this is vkGetSwapchainImagesKHR. */
struct mock_driver {
   uint32_t reported_image_count;
   /* Magic sentinel pointer the driver "writes" to slot N to
    * make OOB-write probing observable in non-ASan builds. */
   void    *fill_pattern;
};

static void mock_get_swapchain_images(struct mock_driver *drv,
      uint32_t *count_io, void **out_images)
{
   if (out_images == NULL)
   {
      /* Count-query call: report the driver's full image count. */
      *count_io = drv->reported_image_count;
      return;
   }

   /* Fill call: write min(driver_count, *count_io) entries.
    * The Vulkan spec is that the count parameter is in/out --
    * caller passes their array capacity, driver writes up to
    * that many entries and updates *count to the number
    * actually written. */
   {
      uint32_t to_write = drv->reported_image_count;
      uint32_t i;
      if (to_write > *count_io)
         to_write = *count_io;
      for (i = 0; i < to_write; i++)
         out_images[i] = drv->fill_pattern;
      *count_io = to_write;
   }
}

/* === verbatim copy of the post-fix request-cap from
 *     gfx/common/vulkan_common.c::vulkan_create_swapchain (the
 *     block immediately after the maxImageCount clamp).  If the
 *     production function amends this clamp, the copy must
 *     follow. === */
static uint32_t cap_desired_request(uint32_t desired_swapchain_images)
{
   if (desired_swapchain_images > VULKAN_MAX_SWAPCHAIN_IMAGES)
      desired_swapchain_images = VULKAN_MAX_SWAPCHAIN_IMAGES;
   return desired_swapchain_images;
}
/* === end verbatim copy === */

/* === verbatim copy of the post-fix returned-count bound from
 *     vulkan_create_swapchain, the block between the two
 *     vkGetSwapchainImagesKHR calls.  If the production function
 *     amends this bound, the copy must follow. === */
static bool returned_count_fits(struct mock_vk_context *ctx)
{
   if (ctx->num_swapchain_images == 0)
      return false;
   if (ctx->num_swapchain_images > VULKAN_MAX_SWAPCHAIN_IMAGES)
      return false;
   return true;
}
/* === end verbatim copy === */

/* === verbatim copy of the acquired-index bounds predicate from
 *     vulkan_acquire_next_image. === */
static bool acquired_index_is_valid(struct mock_vk_context *ctx,
      uint32_t index)
{
   return index < ctx->num_swapchain_images
       && index < VULKAN_MAX_SWAPCHAIN_IMAGES;
}
/* === end verbatim copy === */

static int failures = 0;

/* Drives the post-fix two-call sequence end-to-end against a
 * mock driver, returning the final num_swapchain_images and
 * filling ctx->swapchain_images[].  Mirrors the production
 * sequence exactly.
 *
 * Pre-fix, when reported_count > VULKAN_MAX_SWAPCHAIN_IMAGES the
 * second mock call would write reported_count entries into the
 * fixed-size array -- ASan trips heap-buffer-overflow. Post-fix,
 * the sequence rejects the swapchain before the fill call.
 *
 * The function operates on `ctx` allocated by the caller; the
 * caller is malloc'd at exactly sizeof(struct mock_vk_context)
 * so ASan can flag any write outside that struct's memory.
 * (In production swapchain_images sits in a larger struct, so
 * an OOB write would corrupt sibling fields; here it would
 * corrupt heap metadata or whatever follows the malloc.) */
static bool run_postfix_sequence(struct mock_vk_context *ctx,
      struct mock_driver *drv)
{
   /* First call: query count.  ctx->num_swapchain_images becomes
    * the driver-reported value, which may exceed our capacity. */
   ctx->num_swapchain_images = 0;  /* must initialise before use */
   mock_get_swapchain_images(drv, &ctx->num_swapchain_images, NULL);

   /* Reject the whole swapchain rather than keeping a partial list. */
   if (!returned_count_fits(ctx))
   {
      ctx->num_swapchain_images = 0;
      return false;
   }

   /* Second call: fill. The accepted count is guaranteed to fit. */
   mock_get_swapchain_images(drv, &ctx->num_swapchain_images,
         ctx->swapchain_images);
   return true;
}

/* Probe: well-behaved driver returns 3 images for a request of
 * 3. Common path on Mesa with default settings. Final count is 3. */
static void test_well_behaved_three_images(void)
{
   struct mock_vk_context *ctx = (struct mock_vk_context *)
      calloc(1, sizeof(*ctx));
   struct mock_driver drv = { 3, (void*)0xCAFE };

   if (!ctx) { printf("[ERROR] calloc\n"); failures++; return; }

   if (!run_postfix_sequence(ctx, &drv))
   {
      printf("[ERROR] well-behaved 3-image driver was rejected\n");
      failures++;
      free(ctx);
      return;
   }

   if (ctx->num_swapchain_images != 3)
   {
      printf("[ERROR] well-behaved 3-image driver: got count=%u, expected 3\n",
            ctx->num_swapchain_images);
      failures++;
   }
   else if (   ctx->swapchain_images[0] != (void*)0xCAFE
            || ctx->swapchain_images[2] != (void*)0xCAFE)
   {
      printf("[ERROR] swapchain_images[] not populated correctly\n");
      failures++;
   }
   else
      printf("[SUCCESS] well-behaved 3-image driver: count=3, images filled\n");

   free(ctx);
}

/* Probe: driver returns exactly VULKAN_MAX_SWAPCHAIN_IMAGES.
 * Boundary case -- clamp must NOT fire. */
static void test_at_capacity_boundary(void)
{
   struct mock_vk_context *ctx = (struct mock_vk_context *)
      calloc(1, sizeof(*ctx));
   struct mock_driver drv = { VULKAN_MAX_SWAPCHAIN_IMAGES, (void*)0xBEEF };

   if (!ctx) { printf("[ERROR] calloc\n"); failures++; return; }

   if (!run_postfix_sequence(ctx, &drv))
   {
      printf("[ERROR] at-capacity swapchain was rejected\n");
      failures++;
      free(ctx);
      return;
   }

   if (ctx->num_swapchain_images != VULKAN_MAX_SWAPCHAIN_IMAGES)
   {
      printf("[ERROR] at-capacity boundary: count=%u, expected %u\n",
            ctx->num_swapchain_images,
            (unsigned)VULKAN_MAX_SWAPCHAIN_IMAGES);
      failures++;
   }
   else
      printf("[SUCCESS] at-capacity boundary: count=%u, no spurious clamp\n",
            ctx->num_swapchain_images);

   free(ctx);
}

/* Probe: driver returns 9 images. This is above the old capacity but
 * must remain fully represented by the expanded per-image arrays. */
static void test_driver_returns_nine_images(void)
{
   struct mock_vk_context *ctx = (struct mock_vk_context *)
      calloc(1, sizeof(*ctx));
   struct mock_driver drv = { 9, (void*)0xDEAD };

   if (!ctx) { printf("[ERROR] calloc\n"); failures++; return; }

   if (!run_postfix_sequence(ctx, &drv))
   {
      printf("[ERROR] 9-image swapchain was rejected\n");
      failures++;
      free(ctx);
      return;
   }

   if (ctx->num_swapchain_images != 9)
   {
      printf("[ERROR] 9-image driver: count=%u, expected 9\n",
            ctx->num_swapchain_images);
      failures++;
   }
   else
      printf("[SUCCESS] driver returned %u images, all images retained\n",
            ctx->num_swapchain_images);

   free(ctx);
}

/* Probe: pathological driver returns 64 images. Keeping only 16
 * handles would still allow acquire to return an untracked image,
 * so the complete swapchain must be rejected. */
static void test_driver_returns_many_images(void)
{
   struct mock_vk_context *ctx = (struct mock_vk_context *)
      calloc(1, sizeof(*ctx));
   struct mock_driver drv = { 64, (void*)0xABCD };
   bool accepted;

   if (!ctx) { printf("[ERROR] calloc\n"); failures++; return; }

   accepted = run_postfix_sequence(ctx, &drv);

   if (accepted || ctx->num_swapchain_images != 0)
   {
      printf("[ERROR] 64-image driver was not rejected (count=%u)\n",
            ctx->num_swapchain_images);
      failures++;
   }
   else
      printf("[SUCCESS] 64-image swapchain rejected before array fill\n");

   free(ctx);
}

static void test_driver_returns_no_images(void)
{
   struct mock_vk_context *ctx = (struct mock_vk_context *)
      calloc(1, sizeof(*ctx));
   struct mock_driver drv = { 0, (void*)0xABCD };

   if (!ctx) { printf("[ERROR] calloc\n"); failures++; return; }

   if (run_postfix_sequence(ctx, &drv))
   {
      printf("[ERROR] zero-image swapchain was accepted\n");
      failures++;
   }
   else
      printf("[SUCCESS] zero-image swapchain rejected\n");

   free(ctx);
}

static void test_acquired_index_bounds(void)
{
   struct mock_vk_context ctx;

   memset(&ctx, 0, sizeof(ctx));
   ctx.num_swapchain_images = VULKAN_MAX_SWAPCHAIN_IMAGES;

   if (!acquired_index_is_valid(&ctx, VULKAN_MAX_SWAPCHAIN_IMAGES - 1))
   {
      printf("[ERROR] last valid swapchain image index was rejected\n");
      failures++;
   }
   else if (acquired_index_is_valid(&ctx, VULKAN_MAX_SWAPCHAIN_IMAGES))
   {
      printf("[ERROR] out-of-capacity swapchain image index was accepted\n");
      failures++;
   }
   else
      printf("[SUCCESS] acquired image indices are bounded before array access\n");
}

/* Probe: request-side cap.  cap_desired_request takes a
 * user-configured value (settings->uints.video_max_swapchain_
 * images) and clips it to the array capacity.  Verify the
 * boundary behaviour. */
static void test_request_cap(void)
{
   struct {
      uint32_t input;
      uint32_t expected;
      const char *desc;
   } cases[] = {
      { 1,  1,  "input=1 (under cap)" },
      { 3,  3,  "input=3 (typical)" },
      { 8,  8,  "input=8 (under cap)" },
      { 9,  9,  "input=9 (Mali)" },
      { 16, 16, "input=16 (at cap)" },
      { 17, 16, "input=17 (over cap)" },
      { 64, 16, "input=64 (way over)" },
      { (uint32_t)-1, 16, "input=UINT32_MAX (extreme)" }
   };
   const unsigned n_cases = sizeof(cases) / sizeof(cases[0]);
   unsigned i;
   bool any_fail = false;

   for (i = 0; i < n_cases; i++)
   {
      uint32_t got = cap_desired_request(cases[i].input);
      if (got != cases[i].expected)
      {
         printf("[ERROR] cap_desired_request(%s): got %u, expected %u\n",
               cases[i].desc, got, cases[i].expected);
         failures++;
         any_fail = true;
      }
   }
   if (!any_fail)
      printf("[SUCCESS] cap_desired_request: all %u cases clamp correctly\n",
            n_cases);
}

int main(void)
{
   test_well_behaved_three_images();
   test_at_capacity_boundary();
   test_driver_returns_nine_images();
   test_driver_returns_many_images();
   test_driver_returns_no_images();
   test_acquired_index_bounds();
   test_request_cap();

   if (failures)
   {
      printf("\n%d vulkan_swapchain_clamp test(s) failed\n", failures);
      return 1;
   }
   printf("\nAll vulkan_swapchain_clamp regression tests passed.\n");
   return 0;
}
