/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Based on kmscube example written by Rob Clark, based on test app originally
 * written by Arvin Schnell.
 *
 * Compile and run this with minigbm:
 *
 *   https://chromium.googlesource.com/chromiumos/platform/minigbm
 *
 * Edit the minigbm Makefile to add -DGBM_I915 to CPPFLAGS, then compile and
 * install with make DESTDIR=<some path>. Then pass --with-minigbm=<some path>
 * to configure when configuring vkcube
 */

#define _DEFAULT_SOURCE /* for major() */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <signal.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/major.h>
#include <termios.h>
#include <poll.h>
#include <math.h>
#include <assert.h>
#include <sys/mman.h>
#include <linux/input.h>

#include "common.h"

enum display_mode {
   DISPLAY_MODE_AUTO = 0,
   DISPLAY_MODE_HEADLESS,
   DISPLAY_MODE_KMS,
   DISPLAY_MODE_WAYLAND,
   DISPLAY_MODE_XCB,
   DISPLAY_MODE_KHR,
};

static enum display_mode display_mode = DISPLAY_MODE_AUTO;
static const char *arg_out_file = "./cube.png";

void noreturn
failv(const char *format, va_list args)
{
   vfprintf(stderr, format, args);
   fprintf(stderr, "\n");
   exit(1);
}

void printflike(1,2) noreturn
fail(const char *format, ...)
{
   va_list args;

   va_start(args, format);
   failv(format, args);
   va_end(args);
}

void printflike(2, 3)
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   failv(format, args);
   va_end(args);
}

static char * __attribute__((returns_nonnull))
xstrdup(const char *s)
{
   char *dup = strdup(s);
   if (!dup) {
      fprintf(stderr, "out of memory\n");
      abort();
   }

   return dup;
}

static void
init_vk(struct vkcube *vc, const char *extension)
{
   vkCreateInstance(&(VkInstanceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &(VkApplicationInfo) {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "vkcube",
            .apiVersion = VK_MAKE_VERSION(1, 0, 2),
         },
         .enabledExtensionCount = extension ? 2 : 0,
         .ppEnabledExtensionNames = (const char *[2]) {
            VK_KHR_SURFACE_EXTENSION_NAME,
            extension,
         },
      },
      NULL,
      &vc->instance);

   uint32_t count;
   vkEnumeratePhysicalDevices(vc->instance, &count, NULL);
   fail_if(count == 0, "No Vulkan devices found.\n");
   VkPhysicalDevice pd[count];
   vkEnumeratePhysicalDevices(vc->instance, &count, pd);
   vc->physical_device = pd[0];
   printf("%d physical devices\n", count);

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(vc->physical_device, &properties);
   printf("vendor id %04x, device name %s\n",
          properties.vendorID, properties.deviceName);

   vkGetPhysicalDeviceMemoryProperties(vc->physical_device, &vc->memory_properties);

   vkGetPhysicalDeviceQueueFamilyProperties(vc->physical_device, &count, NULL);
   assert(count > 0);
   VkQueueFamilyProperties props[count];
   vkGetPhysicalDeviceQueueFamilyProperties(vc->physical_device, &count, props);
   assert(props[0].queueFlags & VK_QUEUE_GRAPHICS_BIT);

   vkCreateDevice(vc->physical_device,
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .queueCreateInfoCount = 1,
                     .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                        .queueFamilyIndex = 0,
                        .queueCount = 1,
                        .pQueuePriorities = (float []) { 1.0f },
                     },
                     .enabledExtensionCount = 1,
                     .ppEnabledExtensionNames = (const char * const []) {
                        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                     },
                  },
                  NULL,
                  &vc->device);

   vkGetDeviceQueue(vc->device, 0, 0, &vc->queue);
}

static void
init_vk_objects(struct vkcube *vc)
{
   vkCreateRenderPass(vc->device,
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkAttachmentDescription[]) {
            {
               .format = vc->image_format,
               .samples = 1,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
               .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            }
         },
         .subpassCount = 1,
         .pSubpasses = (VkSubpassDescription []) {
            {
               .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
               .inputAttachmentCount = 0,
               .colorAttachmentCount = 1,
               .pColorAttachments = (VkAttachmentReference []) {
                  {
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  }
               },
               .pResolveAttachments = (VkAttachmentReference []) {
                  {
                     .attachment = VK_ATTACHMENT_UNUSED,
                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  }
               },
               .pDepthStencilAttachment = NULL,
               .preserveAttachmentCount = 0,
               .pPreserveAttachments = NULL,
            }
         },
         .dependencyCount = 0
      },
      NULL,
      &vc->render_pass);

   vc->model.init(vc);

   vkCreateCommandPool(vc->device,
                       &(const VkCommandPoolCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                          .queueFamilyIndex = 0,
                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                       },
                       NULL,
                       &vc->cmd_pool);

   vkCreateSemaphore(vc->device,
                     &(VkSemaphoreCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                     },
                     NULL,
                     &vc->semaphore);
}

static void
init_buffer(struct vkcube *vc, struct vkcube_buffer *b)
{
   vkCreateImageView(vc->device,
                     &(VkImageViewCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        .image = b->image,
                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                        .format = vc->image_format,
                        .components = {
                           .r = VK_COMPONENT_SWIZZLE_R,
                           .g = VK_COMPONENT_SWIZZLE_G,
                           .b = VK_COMPONENT_SWIZZLE_B,
                           .a = VK_COMPONENT_SWIZZLE_A,
                        },
                        .subresourceRange = {
                           .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = 1,
                        },
                     },
                     NULL,
                     &b->view);

   vkCreateFramebuffer(vc->device,
                       &(VkFramebufferCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                          .renderPass = vc->render_pass,
                          .attachmentCount = 1,
                          .pAttachments = &b->view,
                          .width = vc->width,
                          .height = vc->height,
                          .layers = 1
                       },
                       NULL,
                       &b->framebuffer);

   vkCreateFence(vc->device,
                 &(VkFenceCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                    .flags = VK_FENCE_CREATE_SIGNALED_BIT
                 },
                 NULL,
                 &b->fence);

   vkAllocateCommandBuffers(vc->device,
      &(VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = vc->cmd_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &b->cmd_buffer);
}

/* Headless code - write one frame to png */

static void
convert_to_bytes(png_structp png, png_row_infop row_info, png_bytep data)
{
   for (uint32_t i = 0; i < row_info->rowbytes; i += 4) {
      uint8_t *b = &data[i];
      uint32_t pixel;

      memcpy (&pixel, b, sizeof (uint32_t));
      b[0] = (pixel & 0xff0000) >> 16;
      b[1] = (pixel & 0x00ff00) >>  8;
      b[2] = (pixel & 0x0000ff) >>  0;
      b[3] = 0xff;
   }
}

static void
write_png(const char *path, int32_t width, int32_t height, int32_t stride, void *pixels)
{
   FILE *f = NULL;
   png_structp png_writer = NULL;
   png_infop png_info = NULL;

   uint8_t *rows[height];

   for (int32_t y = 0; y < height; y++)
      rows[y] = pixels + y * stride;

   f = fopen(path, "wb");
   fail_if(!f, "failed to open file for writing: %s", path);

   png_writer = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                        NULL, NULL, NULL);
   fail_if (!png_writer, "failed to create png writer");

   png_info = png_create_info_struct(png_writer);
   fail_if(!png_info, "failed to create png writer info");

   png_init_io(png_writer, f);
   png_set_IHDR(png_writer, png_info,
                width, height,
                8, PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT);
   png_write_info(png_writer, png_info);
   png_set_rows(png_writer, png_info, rows);
   png_set_write_user_transform_fn(png_writer, convert_to_bytes);
   png_write_png(png_writer, png_info, PNG_TRANSFORM_IDENTITY, NULL);

   png_destroy_write_struct(&png_writer, &png_info);

   fclose(f);
}

static void
write_buffer(struct vkcube *vc, struct vkcube_buffer *b)
{
   const char *filename = arg_out_file;
   uint32_t mem_size = b->stride * vc->height;
   void *map;

   vkMapMemory(vc->device, b->mem, 0, mem_size, 0, &map);

   fprintf(stderr, "writing first frame to %s\n", filename);
   write_png(filename, vc->width, vc->height, b->stride, map);
}

// Return -1 on failure.
static int
init_headless(struct vkcube *vc)
{
   init_vk(vc, NULL);
   vc->image_format = VK_FORMAT_B8G8R8A8_SRGB;
   init_vk_objects(vc);

   struct vkcube_buffer *b = &vc->buffers[0];

   vkCreateImage(vc->device,
                 &(VkImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = vc->image_format,
                    .extent = { .width = vc->width, .height = vc->height, .depth = 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = 1,
                    .tiling = VK_IMAGE_TILING_LINEAR,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    .flags = 0,
                 },
                 NULL,
                 &b->image);

   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(vc->device, b->image, &requirements);

   vkAllocateMemory(vc->device,
                    &(VkMemoryAllocateInfo) {
                       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                       .allocationSize = requirements.size,
                       .memoryTypeIndex = 0
                    },
                    NULL,
                    &b->mem);

   vkBindImageMemory(vc->device, b->image, b->mem, 0);

   b->stride = vc->width * 4;

   init_buffer(vc, &vc->buffers[0]);

   return 0;
}

#ifdef HAVE_VULKAN_INTEL_H

/* KMS display code - render to kernel modesetting fb */

#include <vulkan/vulkan_intel.h>

static struct termios save_tio;

static void
restore_vt(void)
{
   struct vt_mode mode = { .mode = VT_AUTO };
   ioctl(STDIN_FILENO, VT_SETMODE, &mode);

   tcsetattr(STDIN_FILENO, TCSANOW, &save_tio);
   ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);
}

static void
handle_signal(int sig)
{
   restore_vt();
}

static int
init_vt(struct vkcube *vc)
{
   struct termios tio;
   struct stat buf;
   int ret;

   /* First, save term io setting so we can restore properly. */
   tcgetattr(STDIN_FILENO, &save_tio);

   /* Make sure we're on a vt. */
   ret = fstat(STDIN_FILENO, &buf);
   fail_if(ret == -1, "failed to stat stdin\n");

   if (major(buf.st_rdev) != TTY_MAJOR) {
      fprintf(stderr, "stdin not a vt, running in no-display mode\n");
      return -1;
   }

   atexit(restore_vt);

   /* Set console input to raw mode. */
   tio = save_tio;
   tio.c_lflag &= ~(ICANON | ECHO);
   tcsetattr(STDIN_FILENO, TCSANOW, &tio);

   /* Restore console on SIGINT and friends. */
   struct sigaction act = {
      .sa_handler = handle_signal,
      .sa_flags = SA_RESETHAND
   };
   sigaction(SIGINT, &act, NULL);
   sigaction(SIGSEGV, &act, NULL);
   sigaction(SIGABRT, &act, NULL);

   /* We don't drop drm master, so block VT switching while we're
    * running. Otherwise, switching to X on another VT will crash X when it
    * fails to get drm master. */
   struct vt_mode mode = { .mode = VT_PROCESS, .relsig = 0, .acqsig = 0 };
   ret = ioctl(STDIN_FILENO, VT_SETMODE, &mode);
   fail_if(ret == -1, "failed to take control of vt handling\n");

   /* Set KD_GRAPHICS to disable fbcon while we render. */
   ret = ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS);
   fail_if(ret == -1, "failed to switch console to graphics mode\n");

   return 0;
}

// Return -1 on failure.
static int
init_kms(struct vkcube *vc)
{
   drmModeRes *resources;
   drmModeConnector *connector;
   drmModeEncoder *encoder;
   int i;

   if (init_vt(vc) == -1)
      return -1;

   vc->fd = open("/dev/dri/card0", O_RDWR);
   fail_if(vc->fd == -1, "failed to open /dev/dri/card0\n");

   /* Get KMS resources and find the first active connecter. We'll use that
      connector and the crtc driving it in the mode it's currently running. */
   resources = drmModeGetResources(vc->fd);
   fail_if(!resources, "drmModeGetResources failed: %s\n", strerror(errno));

   for (i = 0; i < resources->count_connectors; i++) {
      connector = drmModeGetConnector(vc->fd, resources->connectors[i]);
      if (connector->connection == DRM_MODE_CONNECTED)
         break;
      drmModeFreeConnector(connector);
      connector = NULL;
   }

   fail_if(!connector, "no connected connector!\n");
   encoder = drmModeGetEncoder(vc->fd, connector->encoder_id);
   fail_if(!encoder, "failed to get encoder\n");
   vc->crtc = drmModeGetCrtc(vc->fd, encoder->crtc_id);
   fail_if(!vc->crtc, "failed to get crtc\n");
   printf("mode info: hdisplay %d, vdisplay %d\n",
          vc->crtc->mode.hdisplay, vc->crtc->mode.vdisplay);

   vc->connector = connector;
   vc->width = vc->crtc->mode.hdisplay;
   vc->height = vc->crtc->mode.vdisplay;

   vc->gbm_device = gbm_create_device(vc->fd);

   init_vk(vc, NULL);
   vc->image_format = VK_FORMAT_R8G8B8A8_SRGB;
   init_vk_objects(vc);

   PFN_vkCreateDmaBufImageINTEL create_dma_buf_image =
      (PFN_vkCreateDmaBufImageINTEL)vkGetDeviceProcAddr(vc->device, "vkCreateDmaBufImageINTEL");

   for (uint32_t i = 0; i < 2; i++) {
      struct vkcube_buffer *b = &vc->buffers[i];
      int fd, stride, ret;

      b->gbm_bo = gbm_bo_create(vc->gbm_device, vc->width, vc->height,
                                GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);

      fd = gbm_bo_get_fd(b->gbm_bo);
      stride = gbm_bo_get_stride(b->gbm_bo);
      create_dma_buf_image(vc->device,
                           &(VkDmaBufImageCreateInfo) {
                              .sType = VK_STRUCTURE_TYPE_DMA_BUF_IMAGE_CREATE_INFO_INTEL,
                              .fd = fd,
                              .format = vc->image_format,
                              .extent = { vc->width, vc->height, 1 },
                              .strideInBytes = stride
                           },
                           NULL,
                           &b->mem,
                           &b->image);
      close(fd);

      b->stride = gbm_bo_get_stride(b->gbm_bo);
      uint32_t bo_handles[4] = { gbm_bo_get_handle(b->gbm_bo).s32, };
      uint32_t pitches[4] = { stride, };
      uint32_t offsets[4] = { 0, };
      ret = drmModeAddFB2(vc->fd, vc->width, vc->height,
                          DRM_FORMAT_XRGB8888, bo_handles,
                          pitches, offsets, &b->fb, 0);
      fail_if(ret == -1, "addfb2 failed\n");

      init_buffer(vc, b);
   }

   return 0;
}

static void
page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
}

static void
mainloop_vt(struct vkcube *vc)
{
   int len, ret;
   char buf[16];
   struct pollfd pfd[2];
   struct vkcube_buffer *b;

   pfd[0].fd = STDIN_FILENO;
   pfd[0].events = POLLIN;
   pfd[1].fd = vc->fd;
   pfd[1].events = POLLIN;

   drmEventContext evctx = {
      .version = 2,
      .page_flip_handler = page_flip_handler,
   };

   ret = drmModeSetCrtc(vc->fd, vc->crtc->crtc_id, vc->buffers[0].fb,
                        0, 0, &vc->connector->connector_id, 1, &vc->crtc->mode);
   fail_if(ret < 0, "modeset failed: %m\n");


   ret = drmModePageFlip(vc->fd, vc->crtc->crtc_id, vc->buffers[0].fb,
                         DRM_MODE_PAGE_FLIP_EVENT, NULL);
   fail_if(ret < 0, "pageflip failed: %m\n");

   while (1) {
      ret = poll(pfd, 2, -1);
      fail_if(ret == -1, "poll failed\n");
      if (pfd[0].revents & POLLIN) {
         len = read(STDIN_FILENO, buf, sizeof(buf));
         switch (buf[0]) {
         case 'q':
            return;
         case '\e':
            if (len == 1)
               return;
         }
      }
      if (pfd[1].revents & POLLIN) {
         drmHandleEvent(vc->fd, &evctx);
         b = &vc->buffers[vc->current & 1];
         vc->model.render(vc, b);

         ret = drmModePageFlip(vc->fd, vc->crtc->crtc_id, b->fb,
                               DRM_MODE_PAGE_FLIP_EVENT, NULL);
         fail_if(ret < 0, "pageflip failed: %m\n");
         vc->current++;
      }
   }
}

#else

static int
init_kms(struct vkcube *vc)
{
   return -1;
}

static void
mainloop_vt(struct vkcube *vc)
{
}

#endif

/* Swapchain-based code - shared between XCB and Wayland */

static VkFormat
choose_surface_format(struct vkcube *vc)
{
   uint32_t num_formats = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(vc->physical_device, vc->surface,
                                        &num_formats, NULL);
   assert(num_formats > 0);

   VkSurfaceFormatKHR formats[num_formats];

   vkGetPhysicalDeviceSurfaceFormatsKHR(vc->physical_device, vc->surface,
                                        &num_formats, formats);

   VkFormat format = VK_FORMAT_UNDEFINED;
   for (int i = 0; i < num_formats; i++) {
      switch (formats[i].format) {
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_SRGB:
         /* These formats are all fine */
         format = formats[i].format;
         break;
      case VK_FORMAT_R8G8B8_SRGB:
      case VK_FORMAT_B8G8R8_SRGB:
      case VK_FORMAT_R5G6B5_UNORM_PACK16:
      case VK_FORMAT_B5G6R5_UNORM_PACK16:
         /* We would like to support these but they don't seem to work. */
      default:
         continue;
      }
   }

   assert(format != VK_FORMAT_UNDEFINED);

   return format;
}

static void
create_swapchain(struct vkcube *vc)
{
   VkSurfaceCapabilitiesKHR surface_caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vc->physical_device, vc->surface,
                                             &surface_caps);
   assert(surface_caps.supportedCompositeAlpha &
          VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);

   VkBool32 supported;
   vkGetPhysicalDeviceSurfaceSupportKHR(vc->physical_device, 0, vc->surface,
                                        &supported);
   assert(supported);

   uint32_t count;
   vkGetPhysicalDeviceSurfacePresentModesKHR(vc->physical_device, vc->surface,
                                             &count, NULL);
   VkPresentModeKHR present_modes[count];
   vkGetPhysicalDeviceSurfacePresentModesKHR(vc->physical_device, vc->surface,
                                             &count, present_modes);
   int i;
   VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
   for (i = 0; i < count; i++) {
      if (present_modes[i] == VK_PRESENT_MODE_FIFO_KHR) {
         present_mode = VK_PRESENT_MODE_FIFO_KHR;
         break;
      }
   }

   uint32_t minImageCount = 2;
   if (minImageCount < surface_caps.minImageCount) {
      if (surface_caps.minImageCount > MAX_NUM_IMAGES)
          fail("surface_caps.minImageCount is too large (is: %d, max: %d)",
               surface_caps.minImageCount, MAX_NUM_IMAGES);
      minImageCount = surface_caps.minImageCount;
   }

   if (surface_caps.maxImageCount > 0 &&
       minImageCount > surface_caps.maxImageCount) {
      minImageCount = surface_caps.maxImageCount;
   }

   vkCreateSwapchainKHR(vc->device,
      &(VkSwapchainCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
         .surface = vc->surface,
         .minImageCount = minImageCount,
         .imageFormat = vc->image_format,
         .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
         .imageExtent = { vc->width, vc->height },
         .imageArrayLayers = 1,
         .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .queueFamilyIndexCount = 1,
         .pQueueFamilyIndices = (uint32_t[]) { 0 },
         .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
         .presentMode = present_mode,
      }, NULL, &vc->swap_chain);

   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &vc->image_count, NULL);
   assert(vc->image_count > 0);
   VkImage swap_chain_images[vc->image_count];
   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &vc->image_count, swap_chain_images);

   assert(vc->image_count <= MAX_NUM_IMAGES);
   for (uint32_t i = 0; i < vc->image_count; i++) {
      vc->buffers[i].image = swap_chain_images[i];
      init_buffer(vc, &vc->buffers[i]);
   }
}

/* XCB display code - render to X window */

static xcb_atom_t
get_atom(struct xcb_connection_t *conn, const char *name)
{
   xcb_intern_atom_cookie_t cookie;
   xcb_intern_atom_reply_t *reply;
   xcb_atom_t atom;

   cookie = xcb_intern_atom(conn, 0, strlen(name), name);
   reply = xcb_intern_atom_reply(conn, cookie, NULL);
   if (reply)
      atom = reply->atom;
   else
      atom = XCB_NONE;

   free(reply);
   return atom;
}

// Return -1 on failure.
static int
init_xcb(struct vkcube *vc)
{
   xcb_screen_iterator_t iter;
   static const char title[] = "Vulkan Cube";

   vc->xcb.conn = xcb_connect(0, 0);
   if (xcb_connection_has_error(vc->xcb.conn))
      return -1;

   vc->xcb.window = xcb_generate_id(vc->xcb.conn);

   uint32_t window_values[] = {
      XCB_EVENT_MASK_EXPOSURE |
      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
      XCB_EVENT_MASK_KEY_PRESS
   };

   iter = xcb_setup_roots_iterator(xcb_get_setup(vc->xcb.conn));

   xcb_create_window(vc->xcb.conn,
                     XCB_COPY_FROM_PARENT,
                     vc->xcb.window,
                     iter.data->root,
                     0, 0,
                     vc->width,
                     vc->height,
                     0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     iter.data->root_visual,
                     XCB_CW_EVENT_MASK, window_values);

   vc->xcb.atom_wm_protocols = get_atom(vc->xcb.conn, "WM_PROTOCOLS");
   vc->xcb.atom_wm_delete_window = get_atom(vc->xcb.conn, "WM_DELETE_WINDOW");
   xcb_change_property(vc->xcb.conn,
                       XCB_PROP_MODE_REPLACE,
                       vc->xcb.window,
                       vc->xcb.atom_wm_protocols,
                       XCB_ATOM_ATOM,
                       32,
                       1, &vc->xcb.atom_wm_delete_window);

   xcb_change_property(vc->xcb.conn,
                       XCB_PROP_MODE_REPLACE,
                       vc->xcb.window,
                       get_atom(vc->xcb.conn, "_NET_WM_NAME"),
                       get_atom(vc->xcb.conn, "UTF8_STRING"),
                       8, // sizeof(char),
                       strlen(title), title);

   xcb_map_window(vc->xcb.conn, vc->xcb.window);

   xcb_flush(vc->xcb.conn);

   init_vk(vc, VK_KHR_XCB_SURFACE_EXTENSION_NAME);

   PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR get_xcb_presentation_support =
      (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)
      vkGetInstanceProcAddr(vc->instance, "vkGetPhysicalDeviceXcbPresentationSupportKHR");
   PFN_vkCreateXcbSurfaceKHR create_xcb_surface =
      (PFN_vkCreateXcbSurfaceKHR)
      vkGetInstanceProcAddr(vc->instance, "vkCreateXcbSurfaceKHR");

   if (!get_xcb_presentation_support(vc->physical_device, 0,
                                     vc->xcb.conn,
                                     iter.data->root_visual)) {
      fail("Vulkan not supported on given X window");
   }

   create_xcb_surface(vc->instance,
      &(VkXcbSurfaceCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
         .connection = vc->xcb.conn,
         .window = vc->xcb.window,
      }, NULL, &vc->surface);

   vc->image_format = choose_surface_format(vc);

   init_vk_objects(vc);

   vc->image_count = 0;

   return 0;
}

static void
schedule_xcb_repaint(struct vkcube *vc)
{
   xcb_client_message_event_t client_message;

   client_message.response_type = XCB_CLIENT_MESSAGE;
   client_message.format = 32;
   client_message.window = vc->xcb.window;
   client_message.type = XCB_ATOM_NOTICE;

   xcb_send_event(vc->xcb.conn, 0, vc->xcb.window,
                  0, (char *) &client_message);
   xcb_flush(vc->xcb.conn);
}

static void
mainloop_xcb(struct vkcube *vc)
{
   xcb_generic_event_t *event;
   xcb_key_press_event_t *key_press;
   xcb_client_message_event_t *client_message;
   xcb_configure_notify_event_t *configure;

   while (1) {
      bool repaint = false;
      event = xcb_wait_for_event(vc->xcb.conn);
      while (event) {
         switch (event->response_type & 0x7f) {
         case XCB_CLIENT_MESSAGE:
            client_message = (xcb_client_message_event_t *) event;
            if (client_message->window != vc->xcb.window)
               break;

            if (client_message->type == vc->xcb.atom_wm_protocols &&
                client_message->data.data32[0] == vc->xcb.atom_wm_delete_window) {
               exit(0);
            }

            if (client_message->type == XCB_ATOM_NOTICE)
               repaint = true;
            break;

         case XCB_CONFIGURE_NOTIFY:
            configure = (xcb_configure_notify_event_t *) event;
            if (vc->width != configure->width ||
                vc->height != configure->height) {
               if (vc->image_count > 0) {
                  vkDestroySwapchainKHR(vc->device, vc->swap_chain, NULL);
                  vc->image_count = 0;
               }

               vc->width = configure->width;
               vc->height = configure->height;
            }
            break;

         case XCB_EXPOSE:
            schedule_xcb_repaint(vc);
            break;

         case XCB_KEY_PRESS:
            key_press = (xcb_key_press_event_t *) event;

            if (key_press->detail == 9)
               exit(0);

            break;
         }
         free(event);

         event = xcb_poll_for_event(vc->xcb.conn);
      }

      if (repaint) {
         if (vc->image_count == 0)
            create_swapchain(vc);

         uint32_t index;
         VkResult result;
         result = vkAcquireNextImageKHR(vc->device, vc->swap_chain, 60,
                                        vc->semaphore, VK_NULL_HANDLE, &index);
         switch (result) {
         case VK_SUCCESS:
            break;
         case VK_NOT_READY: /* try later */
         case VK_TIMEOUT:   /* try later */
         case VK_ERROR_OUT_OF_DATE_KHR: /* handled by native events */
            schedule_xcb_repaint(vc);
            continue;
         default:
            return;
         }

         assert(index <= MAX_NUM_IMAGES);
         vc->model.render(vc, &vc->buffers[index]);

         vkQueuePresentKHR(vc->queue,
             &(VkPresentInfoKHR) {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .swapchainCount = 1,
                .pSwapchains = (VkSwapchainKHR[]) { vc->swap_chain, },
                .pImageIndices = (uint32_t[]) { index, },
                .pResults = &result,
             });

         vkQueueWaitIdle(vc->queue);

         schedule_xcb_repaint(vc);
      }

      xcb_flush(vc->xcb.conn);
   }
}

/* Wayland display code - render to Wayland window */

static void
handle_xdg_surface_configure(void *data, struct xdg_surface *surface,
                             uint32_t serial)
{
   struct vkcube *vc = data;

   xdg_surface_ack_configure(surface, serial);

   if (vc->wl.wait_for_configure) {
      // redraw
      vc->wl.wait_for_configure = false;
   }
}

static const struct xdg_surface_listener xdg_surface_listener = {
   handle_xdg_surface_configure,
};

static void
handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                              int32_t width, int32_t height,
                              struct wl_array *states)
{
}

static void
handle_xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
   handle_xdg_toplevel_configure,
   handle_xdg_toplevel_close,
};

static void
handle_xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
   xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
   handle_xdg_wm_base_ping,
};

static void
handle_wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
			  uint32_t format, int32_t fd, uint32_t size)
{
}

static void
handle_wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
			 uint32_t serial, struct wl_surface *surface,
			 struct wl_array *keys)
{
}

static void
handle_wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
			 uint32_t serial, struct wl_surface *surface)
{
}

static void
handle_wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		       uint32_t serial, uint32_t time, uint32_t key,
		       uint32_t state)
{
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
      exit(0);
}

static void
handle_wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
			     uint32_t serial, uint32_t mods_depressed,
			     uint32_t mods_latched, uint32_t mods_locked,
			     uint32_t group)
{
}

static void
handle_wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
			       int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
   .keymap = handle_wl_keyboard_keymap,
   .enter = handle_wl_keyboard_enter,
   .leave = handle_wl_keyboard_leave,
   .key = handle_wl_keyboard_key,
   .modifiers = handle_wl_keyboard_modifiers,
   .repeat_info = handle_wl_keyboard_repeat_info,
};

static void
handle_wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
			    uint32_t capabilities)
{
   struct vkcube *vc = data;

   if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && (!vc->wl.keyboard)) {
      vc->wl.keyboard = wl_seat_get_keyboard(wl_seat);
      wl_keyboard_add_listener(vc->wl.keyboard, &wl_keyboard_listener, vc);
   } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && vc->wl.keyboard) {
      wl_keyboard_destroy(vc->wl.keyboard);
      vc->wl.keyboard = NULL;
   }
}

static const struct wl_seat_listener wl_seat_listener = {
   handle_wl_seat_capabilities,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
   struct vkcube *vc = data;

   if (strcmp(interface, "wl_compositor") == 0) {
      vc->wl.compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface, 1);
   } else if (strcmp(interface, "xdg_wm_base") == 0) {
      vc->wl.shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener(vc->wl.shell, &xdg_wm_base_listener, vc);
   } else if (strcmp(interface, "wl_seat") == 0) {
      vc->wl.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
      wl_seat_add_listener(vc->wl.seat, &wl_seat_listener, vc);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove
};

// Return -1 on failure.
static int
init_wayland(struct vkcube *vc)
{
   vc->wl.display = wl_display_connect(NULL);
   if (!vc->wl.display)
      return -1;

   vc->wl.seat = NULL;
   vc->wl.keyboard = NULL;
   vc->wl.shell = NULL;

   struct wl_registry *registry = wl_display_get_registry(vc->wl.display);
   wl_registry_add_listener(registry, &registry_listener, vc);

   /* Round-trip to get globals */
   wl_display_roundtrip(vc->wl.display);

   /* We don't need this anymore */
   wl_registry_destroy(registry);

   vc->wl.surface = wl_compositor_create_surface(vc->wl.compositor);

   if (!vc->wl.shell)
      fail("Compositor is missing xdg_wm_base protocol support");

   vc->wl.xdg_surface = xdg_wm_base_get_xdg_surface(vc->wl.shell, vc->wl.surface);

   xdg_surface_add_listener(vc->wl.xdg_surface, &xdg_surface_listener, vc);

   vc->wl.xdg_toplevel = xdg_surface_get_toplevel(vc->wl.xdg_surface);

   xdg_toplevel_add_listener(vc->wl.xdg_toplevel, &xdg_toplevel_listener, vc);
   xdg_toplevel_set_title(vc->wl.xdg_toplevel, "vkcube");

   vc->wl.wait_for_configure = true;
   wl_surface_commit(vc->wl.surface);

   init_vk(vc, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

   PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR get_wayland_presentation_support =
      (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
      vkGetInstanceProcAddr(vc->instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
   PFN_vkCreateWaylandSurfaceKHR create_wayland_surface =
      (PFN_vkCreateWaylandSurfaceKHR)
      vkGetInstanceProcAddr(vc->instance, "vkCreateWaylandSurfaceKHR");

   if (!get_wayland_presentation_support(vc->physical_device, 0,
                                         vc->wl.display)) {
      fail("Vulkan not supported on given Wayland surface");
   }

   create_wayland_surface(vc->instance,
                          &(VkWaylandSurfaceCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
         .display = vc->wl.display,
         .surface = vc->wl.surface,
      }, NULL, &vc->surface);

   vc->image_format = choose_surface_format(vc);

   init_vk_objects(vc);

   create_swapchain(vc);

   return 0;
}

static void
mainloop_wayland(struct vkcube *vc)
{
   VkResult result = VK_SUCCESS;
   struct pollfd fds[] = {
      { wl_display_get_fd(vc->wl.display), POLLIN },
   };
   while (1) {
      uint32_t index;

      while (wl_display_prepare_read(vc->wl.display) != 0)
         wl_display_dispatch_pending(vc->wl.display);
      if (wl_display_flush(vc->wl.display) < 0 && errno != EAGAIN) {
         wl_display_cancel_read(vc->wl.display);
         return;
      }
      if (poll(fds, 1, 0) > 0) {
         wl_display_read_events(vc->wl.display);
         wl_display_dispatch_pending(vc->wl.display);
      } else {
         wl_display_cancel_read(vc->wl.display);
      }

      result = vkAcquireNextImageKHR(vc->device, vc->swap_chain, 60,
                                     vc->semaphore, VK_NULL_HANDLE, &index);
      if (result != VK_SUCCESS)
         return;

      assert(index <= MAX_NUM_IMAGES);
      vc->model.render(vc, &vc->buffers[index]);

      vkQueuePresentKHR(vc->queue,
         &(VkPresentInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = (VkSwapchainKHR[]) { vc->swap_chain, },
            .pImageIndices = (uint32_t[]) { index, },
            .pResults = &result,
         });
      if (result != VK_SUCCESS)
         return;

      vkQueueWaitIdle(vc->queue);
   }
}

static int display_idx = -1;
static int display_mode_idx = -1;
static int display_plane_idx = -1;

// Return -1 on failure.
static int
init_khr(struct vkcube *vc)
{
   init_vk(vc, VK_KHR_DISPLAY_EXTENSION_NAME);
   vc->image_format = VK_FORMAT_B8G8R8A8_SRGB;
   init_vk_objects(vc);

   /* */
   uint32_t display_count = 0;
   vkGetPhysicalDeviceDisplayPropertiesKHR(vc->physical_device,
                                           &display_count, NULL);
   if (!display_count) {
      fprintf(stderr, "No available display\n");
      return -1;
   }

   VkDisplayPropertiesKHR *displays =
      (VkDisplayPropertiesKHR *) malloc(display_count * sizeof(*displays));
   vkGetPhysicalDeviceDisplayPropertiesKHR(vc->physical_device,
                                           &display_count,
                                           displays);

   if (display_idx < 0) {
      for (uint32_t i = 0; i < display_count; i++) {
         fprintf(stdout, "display [%i]:\n", i);
         fprintf(stdout, "   name: %s\n", displays[i].displayName);
         fprintf(stdout, "   physical dimensions: %ux%u\n",
                 displays[i].physicalDimensions.width,
                 displays[i].physicalDimensions.height);
         fprintf(stdout, "   physical resolution: %ux%u\n",
                 displays[i].physicalResolution.width,
                 displays[i].physicalResolution.height);
         fprintf(stdout, "   plane reorder: %s\n",
                 displays[i].planeReorderPossible ? "yes" : "no");
         fprintf(stdout, "   persistent content: %s\n",
                 displays[i].persistentContent ? "yes" : "no");
      }
      free(displays);
      return -1;
   } else if (display_idx >= display_count) {
      fprintf(stderr, "Invalid display index %i/%i\n",
              display_idx, display_count);
      free(displays);
      return -1;
   }

   /* */
   uint32_t mode_count = 0;
   vkGetDisplayModePropertiesKHR(vc->physical_device,
                                 displays[display_idx].display,
                                 &mode_count, NULL);
   if (!mode_count) {
      fprintf(stderr, "Not mode available for display %i (%s)\n",
              display_idx, displays[display_idx].displayName);
      free(displays);
      return -1;
   }
   VkDisplayModePropertiesKHR *modes =
      (VkDisplayModePropertiesKHR *) malloc(mode_count * sizeof(*modes));
   vkGetDisplayModePropertiesKHR(vc->physical_device,
                                 displays[display_idx].display,
                                 &mode_count, modes);
   if (display_mode_idx < 0) {
      fprintf(stdout,  "display [%i] (%s) modes:\n",
              display_idx, displays[display_idx].displayName);
      for (uint32_t i = 0; i < mode_count; i++) {
         fprintf(stdout, "mode [%i]:\n", i);
         fprintf(stdout, "   visible region: %ux%u\n",
                 modes[i].parameters.visibleRegion.width,
                 modes[i].parameters.visibleRegion.height);
         fprintf(stdout, "   refresh rate: %u\n",
                 modes[i].parameters.refreshRate);
      }
      free(displays);
      free(modes);
      return -1;
   } else if (display_mode_idx >= mode_count) {
      fprintf(stderr, "Invalid mode index %i/%i\n",
              display_mode_idx, mode_count);
      free(displays);
      free(modes);
      return -1;
   }

   /* */
   uint32_t plane_count = 0;
   vkGetPhysicalDeviceDisplayPlanePropertiesKHR(vc->physical_device,
                                                &plane_count, NULL);
   if (!plane_count) {
      fprintf(stderr, "Not plane available for display %i (%s)\n",
              display_idx, displays[display_idx].displayName);
      free(displays);
      free(modes);
      return -1;
   }

   VkDisplayPlanePropertiesKHR *planes =
      (VkDisplayPlanePropertiesKHR *) malloc(plane_count * sizeof(*planes));
   vkGetPhysicalDeviceDisplayPlanePropertiesKHR(vc->physical_device,
                                                &plane_count, planes);
   if (display_plane_idx < 0) {
      for (uint32_t i = 0; i < plane_count; i++) {
         fprintf(stdout, "display [%i] (%s) plane [%i]\n",
                 display_idx, displays[display_idx].displayName, i);
         fprintf(stdout, "   current stack index: %u\n",
                 planes[i].currentStackIndex);
         fprintf(stdout, "   displays supported:");
         uint32_t supported_display_count = 0;
         vkGetDisplayPlaneSupportedDisplaysKHR(vc->physical_device,
                                               i, &supported_display_count, NULL);
         VkDisplayKHR *supported_displays =
            (VkDisplayKHR *) malloc(supported_display_count * sizeof(*supported_displays));
         vkGetDisplayPlaneSupportedDisplaysKHR(vc->physical_device,
                                               i, &supported_display_count, supported_displays);
         for (uint32_t j = 0; j < supported_display_count; j++) {
            for (uint32_t k = 0; k < display_count; k++) {
               if (displays[k].display == supported_displays[j]) {
                  fprintf(stdout, " %u", k);
                  break;
               }
            }
         }
         fprintf(stdout, "\n");

         VkDisplayPlaneCapabilitiesKHR plane_caps;
         vkGetDisplayPlaneCapabilitiesKHR(vc->physical_device,
                                          modes[display_mode_idx].displayMode,
                                          display_plane_idx,
                                          &plane_caps);
         fprintf(stdout, "   src pos: %ux%u -> %ux%u\n",
                 plane_caps.minSrcPosition.x,
                 plane_caps.minSrcPosition.y,
                 plane_caps.maxSrcPosition.x,
                 plane_caps.maxSrcPosition.y);
         fprintf(stdout, "   src size: %ux%u -> %ux%u\n",
                 plane_caps.minSrcExtent.width,
                 plane_caps.minSrcExtent.height,
                 plane_caps.maxSrcExtent.width,
                 plane_caps.maxSrcExtent.height);
         fprintf(stdout, "   dst pos: %ux%u -> %ux%u\n",
                 plane_caps.minDstPosition.x,
                 plane_caps.minDstPosition.y,
                 plane_caps.maxDstPosition.x,
                 plane_caps.maxDstPosition.y);
      }
      free(displays);
      free(modes);
      free(planes);
      return -1;
   } else if (display_plane_idx >= plane_count) {
      fprintf(stderr, "Invalid plane index %i/%i\n",
              display_plane_idx, plane_count);
      free(displays);
      free(modes);
      free(planes);
      return -1;
   }

   VkDisplayModeCreateInfoKHR display_mode_create_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR,
      .parameters = modes[display_mode_idx].parameters,
   };
   VkResult result =
      vkCreateDisplayModeKHR(vc->physical_device,
                             displays[display_idx].display,
                             &display_mode_create_info,
                             NULL, &vc->khr.display_mode);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "Unable to create mode\n");
      free(displays);
      free(modes);
      free(planes);
      return -1;
   }

   VkDisplaySurfaceCreateInfoKHR display_plane_surface_create_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = vc->khr.display_mode,
      .planeIndex = display_plane_idx,
      .imageExtent = modes[display_mode_idx].parameters.visibleRegion,
   };
   result =
      vkCreateDisplayPlaneSurfaceKHR(vc->instance,
                                     &display_plane_surface_create_info,
                                     NULL,
                                     &vc->surface);

   vc->width = modes[display_mode_idx].parameters.visibleRegion.width;
   vc->height = modes[display_mode_idx].parameters.visibleRegion.height;

   init_vk_objects(vc);

   create_swapchain(vc);

   free(displays);
   free(modes);
   free(planes);

   return 0;
}

static void
mainloop_khr(struct vkcube *vc)
{
   while (1) {
      uint32_t index;
      VkResult result = vkAcquireNextImageKHR(vc->device, vc->swap_chain, UINT64_MAX,
                                     vc->semaphore, VK_NULL_HANDLE, &index);
      if (result != VK_SUCCESS)
         return;

      assert(index <= MAX_NUM_IMAGES);
      vc->model.render(vc, &vc->buffers[index]);

      vkQueuePresentKHR(vc->queue,
         &(VkPresentInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = (VkSwapchainKHR[]) { vc->swap_chain, },
            .pImageIndices = (uint32_t[]) { index, },
            .pResults = &result,
         });
      if (result != VK_SUCCESS)
         return;

      vkQueueWaitIdle(vc->queue);
   }
}

extern struct model cube_model;

static bool
display_mode_from_string(const char *s, enum display_mode *mode)
{
   if (streq(s, "auto")) {
      *mode = DISPLAY_MODE_AUTO;
      return true;
   } else if (streq(s, "headless")) {
      *mode = DISPLAY_MODE_HEADLESS;
      return true;
   } else if (streq(s, "kms")) {
      *mode = DISPLAY_MODE_KMS;
      return true;
   } else if (streq(s, "wayland")) {
      *mode = DISPLAY_MODE_WAYLAND;
      return true;
   } else if (streq(s, "xcb")) {
      *mode = DISPLAY_MODE_XCB;
      return true;
   } else if (streq(s, "khr")) {
      *mode = DISPLAY_MODE_KHR;
      return true;
   } else {
      return false;
   }
}

static void
print_usage(FILE *f)
{
   const char *usage =
      "usage: vkcube [-n] [-o <file>]\n"
      "\n"
      "  -n                      Don't initialize vt or kms, run headless. This\n"
      "                          option is equivalent to '-m headless'.\n"
      "\n"
      "  -m <mode>               Choose display backend, where <mode> is one of\n"
      "                          \"auto\" (the default), \"headless\", \"khr\",\n"
      "                          \"kms\", \"wayland\", or \"xcb\". This option is\n"
      "                          incompatible with '-n'.\n"
      "\n"
      "  -k <display:mode:plane> Select KHR configuration with 3 number separated\n"
      "                          by the column character. To display the item\n"
      "                          corresponding to those number, just omit the number.\n"
      "\n"
      "  -o <file>               Path to output image when running headless.\n"
      "                          Default is \"./cube.png\".\n"
      ;

   fprintf(f, "%s", usage);
}

static void noreturn printflike(1, 2)
usage_error(const char *fmt, ...)
{
   va_list va;

   fprintf(stderr, "usage error: ");
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
   fprintf(stderr, "\n\n");
   print_usage(stderr);
   exit(EXIT_FAILURE);
}

static void
parse_args(int argc, char *argv[])
{
   /* Setting '+' in the optstring is the same as setting POSIXLY_CORRECT in
    * the enviroment. It tells getopt to stop parsing argv when it encounters
    * the first non-option argument; it also prevents getopt from permuting
    * argv during parsing.
    *
    * The initial ':' in the optstring makes getopt return ':' when an option
    * is missing a required argument.
    */
   static const char *optstring = "+:nm:o:k:";

   int opt;
   bool found_arg_headless = false;
   bool found_arg_display_mode = false;

   while ((opt = getopt(argc, argv, optstring)) != -1) {
      switch (opt) {
      case 'm':
         found_arg_display_mode = true;
         if (!display_mode_from_string(optarg, &display_mode))
            usage_error("option -m given bad display mode");
         break;
      case 'n':
         found_arg_headless = true;
         display_mode = DISPLAY_MODE_HEADLESS;
         break;
      case 'k': {
         char config[40], *saveptr, *t;
         snprintf(config, sizeof(config), "%s", optarg);
         if ((t = strtok_r(config, ":", &saveptr))) {
            display_idx = atoi(t);
            if ((t = strtok_r(NULL, ":", &saveptr))) {
               display_mode_idx = atoi(t);
               if ((t = strtok_r(NULL, ":", &saveptr)))
                  display_plane_idx = atoi(t);
            }
         }
         break;
      }
      case 'o':
         arg_out_file = xstrdup(optarg);
         break;
      case '?':
         usage_error("invalid option '-%c'", optopt);
         break;
      case ':':
         usage_error("option -%c requires an argument", optopt);
         break;
      default:
         assert(!"unreachable");
         break;
      }
   }

   if (found_arg_headless && found_arg_display_mode)
      usage_error("options -n and -m are mutually exclusive");

   if (optind != argc)
      usage_error("trailing args");
}

static void
init_display(struct vkcube *vc)
{
   switch (display_mode) {
   case DISPLAY_MODE_AUTO:
      display_mode = DISPLAY_MODE_WAYLAND;
      if (init_wayland(vc) == -1) {
         fprintf(stderr, "failed to initialize wayland, falling back "
                         "to xcb\n");
         display_mode = DISPLAY_MODE_XCB;
         if (init_xcb(vc) == -1) {
            fprintf(stderr, "failed to initialize xcb, falling back "
                            "to kms\n");
            display_mode = DISPLAY_MODE_KMS;
            if (init_kms(vc) == -1) {
               fprintf(stderr, "failed to initialize xcb, falling "
                               "back to headless\n");
               display_mode = DISPLAY_MODE_HEADLESS;
               if (init_headless(vc) == -1) {
                  fail("failed to initialize headless mode");
               }
            }
         }
      }
      break;
   case DISPLAY_MODE_HEADLESS:
      if (init_headless(vc) == -1)
         fail("failed to initialize headless mode");
      break;
   case DISPLAY_MODE_KHR:
      if (init_khr(vc) == -1)
         fail("fail to initialize khr");
      break;
   case DISPLAY_MODE_KMS:
      if (init_kms(vc) == -1)
         fail("failed to initialize kms");
      break;
   case DISPLAY_MODE_WAYLAND:
      if (init_wayland(vc) == -1)
         fail("failed to initialize wayland");
      break;
   case DISPLAY_MODE_XCB:
      if (init_xcb(vc) == -1)
         fail("failed to initialize xcb");
      break;
   }
}

static void
mainloop(struct vkcube *vc)
{
   switch (display_mode) {
   case DISPLAY_MODE_AUTO:
      assert(!"display mode is unset");
      break;
   case DISPLAY_MODE_WAYLAND:
      mainloop_wayland(vc);
      break;
   case DISPLAY_MODE_XCB:
      mainloop_xcb(vc);
      break;
   case DISPLAY_MODE_KMS:
      mainloop_vt(vc);
      break;
   case DISPLAY_MODE_KHR:
      mainloop_khr(vc);
      break;
   case DISPLAY_MODE_HEADLESS:
      vc->model.render(vc, &vc->buffers[0]);
      write_buffer(vc, &vc->buffers[0]);
      break;
   }
}

int main(int argc, char *argv[])
{
   struct vkcube vc;

   parse_args(argc, argv);

   vc.model = cube_model;
   vc.gbm_device = NULL;
   vc.xcb.window = XCB_NONE;
   vc.wl.surface = NULL;
   vc.width = 1024;
   vc.height = 768;
   gettimeofday(&vc.start_tv, NULL);

   init_display(&vc);
   mainloop(&vc);

   return 0;
}
