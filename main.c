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
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/major.h>
#include <termios.h>
#include <poll.h>
#include <math.h>
#include <assert.h>

#include "common.h"

static void
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   exit(1);
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

   uint32_t count = 1;
   vkEnumeratePhysicalDevices(vc->instance, &count, &vc->physical_device);
   printf("%d physical devices\n", count);

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(vc->physical_device, &properties);
   printf("vendor id %04x, device name %s\n",
          properties.vendorID, properties.deviceName);

   vkCreateDevice(vc->physical_device,
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .queueCreateInfoCount = 1,
                     .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
                        .queueFamilyIndex = 0,
                        .queueCount = 1,
                     }
                  },
                  NULL,
                  &vc->device);

   vkGetDeviceQueue(vc->device, 0, 0, &vc->queue);

   vkCreateRenderPass(vc->device,
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkAttachmentDescription[]) {
            {
               .format = VK_FORMAT_R8G8B8A8_SRGB,
               .samples = 1,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
               .preserveAttachmentCount = 1,
               .pPreserveAttachments = (uint32_t []) { 0 },
            }
         },
         .dependencyCount = 0
      },
      NULL,
      &vc->render_pass);

   vc->model.init(vc);

   vkCreateFence(vc->device,
                 &(VkFenceCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                    .flags = 0
                 },
                 NULL,
                 &vc->fence);

   vkCreateCommandPool(vc->device,
                       &(const VkCommandPoolCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                          .queueFamilyIndex = 0,
                          .flags = 0
                       },
                       NULL,
                       &vc->cmd_pool);
}

static void
init_buffer(struct vkcube *vc, struct vkcube_buffer *b)
{
   vkCreateImageView(vc->device,
                     &(VkImageViewCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        .image = b->image,
                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                        .format = VK_FORMAT_B8G8R8A8_SRGB,
                        .components = {
                           .r = VK_COMPONENT_SWIZZLE_R,
                           .g = VK_COMPONENT_SWIZZLE_G,
                           .b = VK_COMPONENT_SWIZZLE_B,
                           .a = VK_COMPONENT_SWIZZLE_A,
                        },
                        .subresourceRange = {
                           .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = 0,
                           .levelCount = 0,
                           .baseArrayLayer = 0,
                           .layerCount = 1,
                        },
                     },
                     NULL,
                     &b->view);

   vkCreateFramebuffer(vc->device,
                       &(VkFramebufferCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                          .attachmentCount = 1,
                          .pAttachments = &b->view,
                          .width = vc->width,
                          .height = vc->height,
                          .layers = 1
                       },
                       NULL,
                       &b->framebuffer);
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
   static const char filename[] = "cube.png";
   uint32_t mem_size = b->stride * vc->height;
   void *map;

   vkMapMemory(vc->device, b->mem, 0, mem_size, 0, &map);

   fprintf(stderr, "writing first frame to %s\n", filename);
   write_png(filename, vc->width, vc->height, b->stride, map);
}

static void
init_headless(struct vkcube *vc)
{
   init_vk(vc, NULL);

   struct vkcube_buffer *b = &vc->buffers[0];

   vkCreateImage(vc->device,
                 &(VkImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_B8G8R8A8_SRGB,
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
}

/* KMS display code - render to kernel modesetting fb */

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

static void
init_kms(struct vkcube *vc)
{
   drmModeRes *resources;
   drmModeConnector *connector;
   drmModeEncoder *encoder;
   int i;

   if (init_vt(vc) == -1)
      return;

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
                              .format = VK_FORMAT_R8G8B8A8_SRGB,
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
      .version = DRM_EVENT_CONTEXT_VERSION,
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

static void
init_xcb(struct vkcube *vc)
{
   xcb_screen_iterator_t iter;
   static const char title[] = "Vulkan Cube";

   vc->xcb.conn = xcb_connect(0, 0);
   if (xcb_connection_has_error(vc->xcb.conn))
      return;

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

   if (!vkGetPhysicalDeviceXcbPresentationSupportKHR(vc->physical_device, 0,
                                                     vc->xcb.conn,
                                                     iter.data->root_visual)) {
      fprintf(stderr, "Vulkan not supported on given X window");
      abort();
   }

   vkCreateXcbSurfaceKHR(vc->instance,
      &(VkXcbSurfaceCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
         .connection = vc->xcb.conn,
         .window = vc->xcb.window,
      }, NULL, &vc->surface);

   vc->image_count = 0;
}

static void
alloc_buffers_xcb(struct vkcube *vc)
{
   vkCreateSwapchainKHR(vc->device,
      &(VkSwapchainCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
         .surface = vc->surface,
         .minImageCount = 2,
         .imageFormat = VK_FORMAT_B8G8R8A8_SRGB,
         .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
         .imageExtent = { vc->width, vc->height },
         .imageArrayLayers = 1,
         .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .queueFamilyIndexCount = 1,
         .pQueueFamilyIndices = (uint32_t[]) { 0 },
         .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
         .presentMode = VK_PRESENT_MODE_MAILBOX_KHR,
      }, NULL, &vc->swap_chain);

   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &vc->image_count, NULL);
   assert(vc->image_count > 0);
   VkImage swap_chain_images[vc->image_count];
   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &vc->image_count, swap_chain_images);

   for (uint32_t i = 0; i < vc->image_count; i++) {
      vc->buffers[i].image = swap_chain_images[i];
      init_buffer(vc, &vc->buffers[i]);
   }
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
            alloc_buffers_xcb(vc);

         uint32_t index;
         vkAcquireNextImageKHR(vc->device, vc->swap_chain, 60,
                               VK_NULL_HANDLE, VK_NULL_HANDLE, &index);

         vc->model.render(vc, &vc->buffers[index]);

         VkResult result;
         vkQueuePresentKHR(vc->queue,
             &(VkPresentInfoKHR) {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .swapchainCount = 1,
                .pSwapchains = (VkSwapchainKHR[]) { vc->swap_chain, },
                .pImageIndices = (uint32_t[]) { index, },
                .pResults = &result,
             });

         schedule_xcb_repaint(vc);
      }

      xcb_flush(vc->xcb.conn);
   }
}

/* Wayland display code - render to Wayland window */

static void
handle_xdg_surface_configure(void *data, struct xdg_surface *surface,
                             int32_t width, int32_t height,
                             struct wl_array *states, uint32_t serial)
{
   xdg_surface_ack_configure(surface, serial);
}

static void
handle_xdg_surface_delete(void *data, struct xdg_surface *xdg_surface)
{
}

static const struct xdg_surface_listener xdg_surface_listener = {
   handle_xdg_surface_configure,
   handle_xdg_surface_delete,
};

static void
handle_xdg_shell_ping(void *data, struct xdg_shell *shell, uint32_t serial)
{
   xdg_shell_pong(shell, serial);
}

static const struct xdg_shell_listener xdg_shell_listener = {
   handle_xdg_shell_ping,
};

#define XDG_VERSION 5 /* The version of xdg-shell that we implement */

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
   struct vkcube *vc = data;

   if (strcmp(interface, "wl_compositor") == 0) {
      vc->wl.compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface, 1);
   } else if (strcmp(interface, "xdg_shell") == 0) {
      vc->wl.shell = wl_registry_bind(registry, name, &xdg_shell_interface, 1);
      xdg_shell_add_listener(vc->wl.shell, &xdg_shell_listener, vc);
      xdg_shell_use_unstable_version(vc->wl.shell, XDG_VERSION);
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

static void
init_wayland(struct vkcube *vc)
{
   vc->wl.display = wl_display_connect(NULL);
   if (!vc->wl.display)
      return;

   struct wl_registry *registry = wl_display_get_registry(vc->wl.display);
   wl_registry_add_listener(registry, &registry_listener, vc);

   /* Round-trip to get globals */
   wl_display_roundtrip(vc->wl.display);

   /* We don't need this anymore */
   wl_registry_destroy(registry);

   vc->wl.surface = wl_compositor_create_surface(vc->wl.compositor);

   if (!vc->wl.shell) {
      fprintf(stderr, "Compositor is missing unstable xdg_shell v5 protocol support\n");
      abort();
   }

   vc->wl.xdg_surface = xdg_shell_get_xdg_surface(vc->wl.shell,
                                                  vc->wl.surface);
   xdg_surface_add_listener(vc->wl.xdg_surface, &xdg_surface_listener, vc);
   xdg_surface_set_title(vc->wl.xdg_surface, "vkcube");

   init_vk(vc, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

   PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR get_wayland_presentation_support =
      (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
      vkGetInstanceProcAddr(vc->instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
   PFN_vkCreateWaylandSurfaceKHR create_wayland_surface =
      (PFN_vkCreateWaylandSurfaceKHR)
      vkGetInstanceProcAddr(vc->instance, "vkCreateWaylandSurfaceKHR");

   if (!get_wayland_presentation_support(vc->physical_device, 0,
                                         vc->wl.display)) {
      fprintf(stderr, "Vulkan not supported on given Wayland surface");
      abort();
   }

   VkSurfaceKHR wsi_surface;

   create_wayland_surface(vc->instance,
                          &(VkWaylandSurfaceCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
         .display = vc->wl.display,
         .surface = vc->wl.surface,
      }, NULL, &wsi_surface);

   uint32_t num_formats = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(vc->physical_device, wsi_surface,
                                        &num_formats, NULL);
   assert(num_formats > 0);

   VkSurfaceFormatKHR formats[num_formats];

   vkGetPhysicalDeviceSurfaceFormatsKHR(vc->physical_device, wsi_surface,
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

   vkCreateSwapchainKHR(vc->device,
      &(VkSwapchainCreateInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
         .surface = wsi_surface,
         .minImageCount = 2,
         .imageFormat = format,
         .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
         .imageExtent = { vc->width, vc->height },
         .imageArrayLayers = 1,
         .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .queueFamilyIndexCount = 1,
         .pQueueFamilyIndices = (uint32_t[]) { 0 },
         .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
         .presentMode = VK_PRESENT_MODE_MAILBOX_KHR,
      }, NULL, &vc->swap_chain);

   uint32_t image_count;
   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &image_count, NULL);
   assert(image_count > 0);

   VkImage swap_chain_images[image_count];
   vkGetSwapchainImagesKHR(vc->device, vc->swap_chain,
                           &image_count, swap_chain_images);

   for (uint32_t i = 0; i < image_count; i++) {
      vc->buffers[i].image = swap_chain_images[i];
      init_buffer(vc, &vc->buffers[i]);
   }
}

static void
mainloop_wayland(struct vkcube *vc)
{
   VkResult result = VK_SUCCESS;
   while (1) {
      uint32_t index;
      result = vkAcquireNextImageKHR(vc->device, vc->swap_chain, 60,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE, &index);
      if (result != VK_SUCCESS)
         return;

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
   }
}

extern struct model cube_model;

int main(int argc, char *argv[])
{
   struct vkcube vc;
   bool headless;

   vc.model = cube_model;
   vc.gbm_device = NULL;
   vc.xcb.window = XCB_NONE;
   vc.wl.surface = NULL;
   vc.width = 1024;
   vc.height = 768;
   gettimeofday(&vc.start_tv, NULL);

   if (argc > 1) {
      if (strcmp(argv[1], "-n") == 0) {
         headless = true;
      } else {
         fprintf(stderr, "usage: vkcube [-n]\n\n"
		 "  -n  Don't initialize vt or kms, run headless.\n");
	 exit(1);
      }
   }

   if (headless) {
      init_headless(&vc);
   } else {
      init_wayland(&vc);
      if (vc.wl.surface == NULL) {
         init_xcb(&vc);
         if (vc.xcb.window == XCB_NONE) {
            init_kms(&vc);
         }
      }
   }

   if (vc.wl.surface) {
      mainloop_wayland(&vc);
   } else if (vc.xcb.window) {
      mainloop_xcb(&vc);
   } else if (vc.gbm_device) {
      mainloop_vt(&vc);
   } else {
      vc.model.render(&vc, &vc.buffers[0]);
      write_buffer(&vc, &vc.buffers[0]);
   }

   return 0;
}
