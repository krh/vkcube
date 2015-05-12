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
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <libpng16/png.h>

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_intel.h>

#include <gbm.h>

#include "esUtil.h"

struct vkcube {
   int fd;
   struct gbm_device *gbm_device;

   drmModeCrtc *crtc;
   drmModeConnector *connector;
   uint32_t width, height;

   VkInstance instance;
   VkDevice device;
   VkQueue queue;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkDeviceMemory mem;
   VkBuffer buffer;
   VkBufferView ubo_view;
   VkDynamicVpState vp_state;
   VkDynamicRsState rs_state;
   VkDescriptorSet descriptor_set;

   void *map;
   uint32_t vertex_offset, colors_offset, normals_offset;
   bool no_display;
};

struct vkcube_buffer {
   struct gbm_bo *gbm_bo;
   VkDeviceMemory mem;
   VkImage image;
   VkColorAttachmentView view;
   VkFramebuffer framebuffer;
   uint32_t fb;
   uint32_t stride;
};


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
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   restore_vt();

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   exit(1);
}

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
handle_signal(int sig)
{
   restore_vt();
}

static void
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
      vc->no_display = true;
      return;
   }

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
}

static void
init_kms(struct vkcube *vc)
{
   if (vc->no_display) {
      vc->width = 1024;
      vc->height = 768;
      vc->fd = open("/dev/dri/renderD128", O_RDWR);
      fail_if(vc->fd == -1, "failed to open /dev/dri/renderD128\n");
      vc->gbm_device = gbm_create_device(vc->fd);
   } else {
      drmModeRes *resources;
      drmModeConnector *connector;
      drmModeEncoder *encoder;
      int i;

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
   }
}

struct ubo {
   ESMatrix modelview;
   ESMatrix modelviewprojection;
   float normal[12];
};

static void
init_vk(struct vkcube *vc)
{
   vkCreateInstance(&(VkInstanceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pAppInfo = &(VkApplicationInfo) {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pAppName = "vkcube",
            .apiVersion = 1
         }
      },
      &vc->instance);

   uint32_t count = 1;
   VkPhysicalDevice physicalDevices[1];
   vkEnumeratePhysicalDevices(vc->instance, &count, physicalDevices);
   printf("%d physical devices\n", count);

   VkPhysicalDeviceProperties properties;
   size_t size = sizeof(properties);
   vkGetPhysicalDeviceInfo(physicalDevices[0],
                           VK_PHYSICAL_DEVICE_INFO_TYPE_PROPERTIES,
                           &size, &properties);
   printf("vendor id %04x, device name %s\n",
          properties.vendorId, properties.deviceName);

   vkCreateDevice(physicalDevices[0],
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .queueRecordCount = 1,
                     .pRequestedQueues = &(VkDeviceQueueCreateInfo) {
                        .queueNodeIndex = 0,
                        .queueCount = 1                       
                     }
                  },
                  &vc->device);

   vkGetDeviceQueue(vc->device, 0, 0, &vc->queue);

   VkDescriptorSetLayout set_layout;
   vkCreateDescriptorSetLayout(vc->device,
                               &(VkDescriptorSetLayoutCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                  .count = 1,
                                  .pBinding = (VkDescriptorSetLayoutBinding[]) {
                                     {
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                        .count = 1,
                                        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                                        .pImmutableSamplers = NULL
                                     }
                                  }
                               },
                               &set_layout);

   vkCreatePipelineLayout(vc->device,
                          &(VkPipelineLayoutCreateInfo) {
                             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                             .descriptorSetCount = 1,
                             .pSetLayouts = &set_layout,
                          },
                          &vc->pipeline_layout);

   VkPipelineVertexInputCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO,
      .bindingCount = 3,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 3 * sizeof(float),
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 3 * sizeof(float),
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 2,
            .strideInBytes = 3 * sizeof(float),
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         }
      },
      .attributeCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            .location = 1,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            .location = 2,
            .binding = 2,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offsetInBytes = 0
         }
      }      
   };

   VkPipelineIaStateCreateInfo ia_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO,
      .pNext = &vi_create_info,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .disableVertexReuse = false,
      .primitiveRestartEnable = false,
      .primitiveRestartIndex = 0
   };

#define GLSL(src) "#version 330\n" #src

   static const char vs_source[] = GLSL(
      layout(set = 0, index = 0) uniform block {
          uniform mat4 modelviewMatrix;
          uniform mat4 modelviewprojectionMatrix;
          uniform mat3 normalMatrix;
      };
      
      layout(location = 0) in vec4 in_position;
      layout(location = 1) in vec4 in_color;
      layout(location = 2) in vec3 in_normal;
      
      vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);
      
      out vec4 vVaryingColor;
      
      void main()
      {
          gl_Position = modelviewprojectionMatrix * in_position;
          vec3 vEyeNormal = normalMatrix * in_normal;
          vec4 vPosition4 = modelviewMatrix * in_position;
          vec3 vPosition3 = vPosition4.xyz / vPosition4.w;
          vec3 vLightDir = normalize(lightSource.xyz - vPosition3);
          float diff = max(0.0, dot(vEyeNormal, vLightDir));
          vVaryingColor = vec4(diff * in_color.rgb, 1.0);
      });

   static const char fs_source[] = GLSL(
      in vec4 vVaryingColor;
      
      void main()
      {
          gl_FragColor = vVaryingColor;
      });

   VkShader vs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .codeSize = sizeof(vs_source),
                     .pCode = vs_source,
                     .flags = 0
                  },
                  &vs);

   VkShader fs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .codeSize = sizeof(fs_source),
                     .pCode = fs_source,
                     .flags = 0
                  },
                  &fs);

   VkPipelineShaderStageCreateInfo vs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &ia_create_info,
      .shader = {
         .stage = VK_SHADER_STAGE_VERTEX,
         .shader = vs,
         .linkConstBufferCount = 0,
         .pLinkConstBufferInfo = NULL,
         .pSpecializationInfo = NULL
      }
   };

   VkPipelineShaderStageCreateInfo fs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &vs_create_info,
      .shader = {
         .stage = VK_SHADER_STAGE_FRAGMENT,
         .shader = fs,
         .linkConstBufferCount = 0,
         .pLinkConstBufferInfo = NULL,
         .pSpecializationInfo = NULL
      }
   };

   VkPipelineRsStateCreateInfo rs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RS_STATE_CREATE_INFO,
      .pNext = &fs_create_info,
      .depthClipEnable = true,
      .rasterizerDiscardEnable = false,
      .fillMode = VK_FILL_MODE_SOLID,
      .cullMode = VK_CULL_MODE_BACK,
      .frontFace = VK_FRONT_FACE_CW
   };

   vkCreateGraphicsPipeline(vc->device,
                            &(VkGraphicsPipelineCreateInfo) {
                               .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                               .pNext = &rs_create_info,
                               .flags = 0,
                               .layout = vc->pipeline_layout
                            },
                            &vc->pipeline);

   static const float vVertices[] = {
      // front
      -1.0f, -1.0f, +1.0f, // point blue
      +1.0f, -1.0f, +1.0f, // point magenta
      -1.0f, +1.0f, +1.0f, // point cyan
      +1.0f, +1.0f, +1.0f, // point white
      // back
      +1.0f, -1.0f, -1.0f, // point red
      -1.0f, -1.0f, -1.0f, // point black
      +1.0f, +1.0f, -1.0f, // point yellow
      -1.0f, +1.0f, -1.0f, // point green
      // right
      +1.0f, -1.0f, +1.0f, // point magenta
      +1.0f, -1.0f, -1.0f, // point red
      +1.0f, +1.0f, +1.0f, // point white
      +1.0f, +1.0f, -1.0f, // point yellow
      // left
      -1.0f, -1.0f, -1.0f, // point black
      -1.0f, -1.0f, +1.0f, // point blue
      -1.0f, +1.0f, -1.0f, // point green
      -1.0f, +1.0f, +1.0f, // point cyan
      // top
      -1.0f, +1.0f, +1.0f, // point cyan
      +1.0f, +1.0f, +1.0f, // point white
      -1.0f, +1.0f, -1.0f, // point green
      +1.0f, +1.0f, -1.0f, // point yellow
      // bottom
      -1.0f, -1.0f, -1.0f, // point black
      +1.0f, -1.0f, -1.0f, // point red
      -1.0f, -1.0f, +1.0f, // point blue
      +1.0f, -1.0f, +1.0f  // point magenta
   };

   static const float vColors[] = {
      // front
      0.0f,  0.0f,  1.0f, // blue
      1.0f,  0.0f,  1.0f, // magenta
      0.0f,  1.0f,  1.0f, // cyan
      1.0f,  1.0f,  1.0f, // white
      // back
      1.0f,  0.0f,  0.0f, // red
      0.0f,  0.0f,  0.0f, // black
      1.0f,  1.0f,  0.0f, // yellow
      0.0f,  1.0f,  0.0f, // green
      // right
      1.0f,  0.0f,  1.0f, // magenta
      1.0f,  0.0f,  0.0f, // red
      1.0f,  1.0f,  1.0f, // white
      1.0f,  1.0f,  0.0f, // yellow
      // left
      0.0f,  0.0f,  0.0f, // black
      0.0f,  0.0f,  1.0f, // blue
      0.0f,  1.0f,  0.0f, // green
      0.0f,  1.0f,  1.0f, // cyan
      // top
      0.0f,  1.0f,  1.0f, // cyan
      1.0f,  1.0f,  1.0f, // white
      0.0f,  1.0f,  0.0f, // green
      1.0f,  1.0f,  0.0f, // yellow
      // bottom
      0.0f,  0.0f,  0.0f, // black
      1.0f,  0.0f,  0.0f, // red
      0.0f,  0.0f,  1.0f, // blue
      1.0f,  0.0f,  1.0f  // magenta
   };

   static const float vNormals[] = {
      // front
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      // back
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      // top
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      // bottom
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f  // down
   };

   vc->vertex_offset = sizeof(struct ubo);
   vc->colors_offset = vc->vertex_offset + sizeof(vVertices);
   vc->normals_offset = vc->colors_offset + sizeof(vColors);
   uint32_t mem_size = vc->normals_offset + sizeof(vNormals);

   vkAllocMemory(vc->device,
                 &(VkMemoryAllocInfo) {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                    .allocationSize = mem_size,
                    .memProps = VK_MEMORY_PROPERTY_HOST_DEVICE_COHERENT_BIT,
                    .memPriority = VK_MEMORY_PRIORITY_NORMAL
                 },
                 &vc->mem);

   vkMapMemory(vc->device, vc->mem, 0, mem_size, 0, &vc->map);
   memcpy(vc->map + vc->vertex_offset, vVertices, sizeof(vVertices));
   memcpy(vc->map + vc->colors_offset, vColors, sizeof(vColors));
   memcpy(vc->map + vc->normals_offset, vNormals, sizeof(vNormals));

   vkCreateBuffer(vc->device,
                  &(VkBufferCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .size = mem_size,
                     .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     .flags = 0
                  },
                  &vc->buffer);

   vkQueueBindObjectMemory(vc->queue, VK_OBJECT_TYPE_BUFFER,
                           vc->buffer,
                           0, /* allocation index; for objects which need to
                               * bind to multiple mems */
                           vc->mem, 0);

   vkCreateBufferView(vc->device,
                      &(VkBufferViewCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                         .buffer = vc->buffer,
                         .viewType = VK_BUFFER_VIEW_TYPE_RAW,
                         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                         .offset = 0,
                         .range = sizeof(struct ubo)
                      },
                      &vc->ubo_view);

   vkCreateDynamicViewportState(vc->device,
                                &(VkDynamicVpStateCreateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_DYNAMIC_VP_STATE_CREATE_INFO,
                                   .viewportAndScissorCount = 1,
                                   .pViewports = (VkViewport[]) {
                                      {
                                         .originX = 0,
                                         .originY = 0,
                                         .width = vc->width,
                                         .height = vc->height,
                                         .minDepth = 0,
                                         .maxDepth = 1
                                      }
                                   },
                                   .pScissors = (VkRect[]) {
                                      { {  0,  0 }, { vc->width, vc->height } },
                                   }
                                },
                                &vc->vp_state);

   vkCreateDynamicRasterState(vc->device,
                              &(VkDynamicRsStateCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_DYNAMIC_RS_STATE_CREATE_INFO,
                              },
                              &vc->rs_state);   

   vkAllocDescriptorSets(vc->device, 0 /* pool */,
                         VK_DESCRIPTOR_SET_USAGE_STATIC,
                         1, &set_layout, &vc->descriptor_set, &count);

   vkUpdateDescriptors(vc->device, vc->descriptor_set, 1,
                       (const void * []) {
                          &(VkUpdateBuffers) {
                             .sType = VK_STRUCTURE_TYPE_UPDATE_BUFFERS,
                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             .arrayIndex = 0,
                             .binding = 0,
                             .count = 1,
                             .pBufferViews = (VkBufferViewAttachInfo[]) {
                                {
                                   .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_ATTACH_INFO,
                                   .view = vc->ubo_view,
                                }
                             }
                          }
                       });
}

static void
init_buffer(struct vkcube *vc, struct vkcube_buffer *b)
{
   uint32_t stride;
   int fd, ret;

   if (vc->no_display) {
      vkCreateImage(vc->device,
                    &(VkImageCreateInfo) {
                       .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                       .imageType = VK_IMAGE_TYPE_2D,
                       .format = VK_FORMAT_B8G8R8A8_UNORM,
                       .extent = { .width = vc->width, .height = vc->height, .depth = 1 },
                       .mipLevels = 1,
                       .arraySize = 1,
                       .samples = 1,
                       .tiling = VK_IMAGE_TILING_LINEAR,
                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                       .flags = 0,
                    },
                    &b->image);

      VkMemoryRequirements requirements;
      size_t size = sizeof(requirements);
      vkGetObjectInfo(vc->device, VK_OBJECT_TYPE_IMAGE, b->image,
                      VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS,
                      &size, &requirements);

      vkAllocMemory(vc->device,
                    &(VkMemoryAllocInfo) {
                       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                       .allocationSize = requirements.size,
                       .memProps = VK_MEMORY_PROPERTY_HOST_DEVICE_COHERENT_BIT,
                       .memPriority = VK_MEMORY_PRIORITY_NORMAL
                    },
                    &b->mem);

      vkQueueBindObjectMemory(vc->queue, VK_OBJECT_TYPE_IMAGE,
                              b->image, 0, b->mem, 0);

      b->stride = vc->width * 4;
   } else {
      b->gbm_bo = gbm_bo_create(vc->gbm_device, vc->width, vc->height,
                                GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);

      fd = gbm_bo_get_fd(b->gbm_bo);
      stride = gbm_bo_get_stride(b->gbm_bo);
      vkCreateDmaBufImageINTEL(vc->device,
                               &(VkDmaBufImageCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_DMA_BUF_IMAGE_CREATE_INFO_INTEL,
                                  .fd = fd,
                                  .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .extent = { vc->width, vc->height, 1 },
                                  .strideInBytes = stride
                                },
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
   }

   vkCreateColorAttachmentView(vc->device,
                               &(VkColorAttachmentViewCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_COLOR_ATTACHMENT_VIEW_CREATE_INFO,
                                  .image = b->image,
                                  .format = VK_FORMAT_B8G8R8A8_UNORM,
                                  .mipLevel = 0,
                                  .baseArraySlice = 0,
                                  .arraySize = 1,
                                  .msaaResolveImage = 0,
                                  .msaaResolveSubResource = { 0, }
                               },
                               &b->view);
   
   vkCreateFramebuffer(vc->device,
                       &(VkFramebufferCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = (VkColorAttachmentBindInfo[]) {
                             {
                                .view = b->view,
                                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                             }
                          },
                          .pDepthStencilAttachment = NULL,
                          .sampleCount = 1,
                          .width = vc->width,
                          .height = vc->height,
                          .layers = 1
                       },
                       &b->framebuffer);
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
render_cube_frame(struct vkcube *vc, struct vkcube_buffer *b, int i)
{
   struct ubo ubo;

   esMatrixLoadIdentity(&ubo.modelview);
   esTranslate(&ubo.modelview, 0.0f, 0.0f, -8.0f);
   esRotate(&ubo.modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
   esRotate(&ubo.modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
   esRotate(&ubo.modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

   float aspect = (float) vc->height / (float) vc->width;
   ESMatrix projection;
   esMatrixLoadIdentity(&projection);
   esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 6.0f, 10.0f);

   esMatrixLoadIdentity(&ubo.modelviewprojection);
   esMatrixMultiply(&ubo.modelviewprojection, &ubo.modelview, &projection);

   /* The mat3 normalMatrix is laid out as 3 vec4s. */
   memcpy(ubo.normal, &ubo.modelview, sizeof ubo.normal);

   memcpy(vc->map, &ubo, sizeof(ubo));

   VkRenderPass render_pass;
   vkCreateRenderPass(vc->device,
                      &(VkRenderPassCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                         .renderArea = { { 0, 0 }, { vc->width, vc->height } },
                         .colorAttachmentCount = 1,
                         .extent = { },
                         .sampleCount = 1,
                         .layers = 1,
                         .pColorFormats = (VkFormat[]) { VK_FORMAT_R8G8B8A8_UNORM },
                         .pColorLayouts = (VkImageLayout[]) { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
                         .pColorLoadOps = (VkAttachmentLoadOp[]) { VK_ATTACHMENT_LOAD_OP_CLEAR },
                         .pColorStoreOps = (VkAttachmentStoreOp[]) { VK_ATTACHMENT_STORE_OP_STORE },
                         .pColorLoadClearValues = (VkClearColor[]) {
                            { .color = { .floatColor = { 0.2, 0.2, 0.2, 1.0 } }, .useRawValue = false }
                         },
                         .depthStencilFormat = VK_FORMAT_UNDEFINED,
                      },
                      &render_pass);

   VkCmdBuffer cmd_buffer;
   vkCreateCommandBuffer(vc->device,
                         &(VkCmdBufferCreateInfo) {
                            .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_CREATE_INFO,
                            .queueNodeIndex = 0,
                            .flags = 0
                         },
                         &cmd_buffer);

   vkBeginCommandBuffer(cmd_buffer,
                        &(VkCmdBufferBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO,
                           .flags = 0
                        });

   vkCmdBeginRenderPass(cmd_buffer,
                        &(VkRenderPassBegin) {
                           .renderPass = render_pass,
                           .framebuffer = b->framebuffer
                        });

   vkCmdBindVertexBuffers(cmd_buffer, 0, 3,
                          (VkBuffer[]) {
                             vc->buffer,
                             vc->buffer,
                             vc->buffer
                          },
                          (VkDeviceSize[]) {
                             vc->vertex_offset,
                             vc->colors_offset,
                             vc->normals_offset
                           });

   vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vc->pipeline);

   vkCmdBindDescriptorSets(cmd_buffer,
                           VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1,
                           &vc->descriptor_set, 0, NULL);

   vkCmdBindDynamicStateObject(cmd_buffer,
                               VK_STATE_BIND_POINT_VIEWPORT, vc->vp_state);
   vkCmdBindDynamicStateObject(cmd_buffer,
                               VK_STATE_BIND_POINT_RASTER, vc->rs_state);

   vkCmdDraw(cmd_buffer, 0, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 4, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 8, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 12, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 16, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 20, 4, 0, 1);

   vkCmdEndRenderPass(cmd_buffer, render_pass);

   vkEndCommandBuffer(cmd_buffer);

   vkQueueSubmit(vc->queue, 1, &cmd_buffer, 0);

   vkQueueWaitIdle(vc->queue);

   vkDestroyObject(vc->device, VK_OBJECT_TYPE_COMMAND_BUFFER, cmd_buffer);
   vkDestroyObject(vc->device, VK_OBJECT_TYPE_RENDER_PASS, render_pass);
}

static void
present(struct vkcube *vc, struct vkcube_buffer *b, int i)
{
   render_cube_frame(vc, b, i);

   if (vc->no_display) {
      write_buffer(vc, b);
   } else {
      drmModePageFlip(vc->fd, vc->crtc->crtc_id, b->fb,
                      DRM_MODE_PAGE_FLIP_EVENT, NULL);
   }
}

static void
page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
}

int main(int argc, char *argv[])
{
   struct vkcube vc;
   struct vkcube_buffer b[2];
   int ret, i = 0;

   vc.no_display = false;
   if (argc > 1) {
      if (strcmp(argv[1], "-n") == 0) {
         vc.no_display = true;
      } else {
         fprintf(stderr, "usage: vkcube [-n]\n\n"
		 "  -n  Don't initialize vt or kms, run headless.\n");
	 exit(1);
      }
   }

   if (!vc.no_display)
      init_vt(&vc);

   init_kms(&vc);

   init_vk(&vc);

   init_buffer(&vc, &b[0]);
   init_buffer(&vc, &b[1]);

   present(&vc, &b[i], i);
   i++;

   if (vc.no_display)
      exit(0);

   int len;
   char buf[16];
   struct pollfd pfd[2];
   pfd[0].fd = STDIN_FILENO;
   pfd[0].events = POLLIN;
   pfd[1].fd = vc.fd;
   pfd[1].events = POLLIN;

   drmEventContext evctx = {
      .version = DRM_EVENT_CONTEXT_VERSION,
      .page_flip_handler = page_flip_handler,
   };

   do {
      ret = poll(pfd, 2, -1);
      fail_if(ret == -1, "poll failed\n");
      if (pfd[0].revents & POLLIN) {
         len = read(STDIN_FILENO, buf, sizeof(buf));
         switch (buf[0]) {
         case 'q':
            goto out;
         case '\e':
            if (len == 1)
               goto out;
            break;
         }
      }
      if (pfd[1].revents & POLLIN) {
         drmHandleEvent(vc.fd, &evctx);
         present(&vc, &b[i & 1], i);
         i++;
      }
   } while (1);

 out:
   restore_vt();

   return 0;
}
