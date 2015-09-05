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
#include <sys/time.h>
#include <math.h>
#include <assert.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <libpng16/png.h>

#include <xcb/xcb.h>

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_intel.h>
#include <vulkan/vk_wsi_swapchain.h>
#include <vulkan/vk_wsi_device_swapchain.h>

#include <gbm.h>

#include "esUtil.h"

#define MAX_NUM_IMAGES 4

struct vkcube_buffer {
   struct gbm_bo *gbm_bo;
   VkDeviceMemory mem;
   VkImage image;
   VkAttachmentView view;
   VkFramebuffer framebuffer;
   uint32_t fb;
   uint32_t stride;
};

struct vkcube {
   int fd;
   struct gbm_device *gbm_device;

   struct {
      xcb_connection_t *conn;
      xcb_window_t window;
      xcb_atom_t atom_wm_protocols;
      xcb_atom_t atom_wm_delete_window;
   } xcb;

   struct {
      struct wl_display *display;
      struct wl_compositor *compositor;
      struct xdg_shell *shell;
      struct wl_surface *surface;
      struct xdg_surface *xdg_surface;
   } wl;

   VkSwapChainWSI swap_chain;

   drmModeCrtc *crtc;
   drmModeConnector *connector;
   uint32_t width, height;

   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkRenderPass render_pass;
   VkQueue queue;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkDeviceMemory mem;
   VkBuffer buffer;
   VkBufferView ubo_view;
   VkDynamicViewportState vp_state;
   VkDynamicRasterState rs_state;
   VkDynamicColorBlendState cb_state;
   VkDescriptorSet descriptor_set;
   VkFence fence;
   VkCmdPool cmd_pool;

   void *map;
   uint32_t vertex_offset, colors_offset, normals_offset;

   struct timeval start_tv;
   struct vkcube_buffer buffers[MAX_NUM_IMAGES];
   int current;
};

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
   vkEnumeratePhysicalDevices(vc->instance, &count, &vc->physical_device);
   printf("%d physical devices\n", count);

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(vc->physical_device, &properties);
   printf("vendor id %04x, device name %s\n",
          properties.vendorId, properties.deviceName);

   vkCreateDevice(vc->physical_device,
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .queueRecordCount = 1,
                     .pRequestedQueues = &(VkDeviceQueueCreateInfo) {
                        .queueFamilyIndex = 0,
                        .queueCount = 1,
                     }
                  },
                  &vc->device);

   vkGetDeviceQueue(vc->device, 0, 0, &vc->queue);

   vkCreateRenderPass(vc->device,
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkAttachmentDescription[]) {
            {
               .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION,
               .format = VK_FORMAT_R8G8B8A8_UNORM,
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
               .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION,
               .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
               .inputCount = 0,
               .colorCount = 1,
               .colorAttachments = (VkAttachmentReference []) {
                  {
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  }
               },
               .resolveAttachments = (VkAttachmentReference []) {
                  {
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  }
               },
               .depthStencilAttachment = (VkAttachmentReference) {
                  .attachment = VK_ATTACHMENT_UNUSED
               },
               .preserveCount = 1,
               .preserveAttachments = (VkAttachmentReference []) {
                  {
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  }
               },
            }
         },
         .dependencyCount = 0
      },
      &vc->render_pass);

   VkDescriptorSetLayout set_layout;
   vkCreateDescriptorSetLayout(vc->device,
                               &(VkDescriptorSetLayoutCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                  .count = 1,
                                  .pBinding = (VkDescriptorSetLayoutBinding[]) {
                                     {
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                        .arraySize = 1,
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

   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
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

   VkShaderModule vs_module;
   vkCreateShaderModule(vc->device,
                        &(VkShaderModuleCreateInfo) {
                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                           .codeSize = sizeof(vs_source),
                           .pCode = vs_source,
                        },
                        &vs_module);

   VkShader vs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .module = vs_module,
                     .pName = "main",
                  },
                  &vs);

   VkShaderModule fs_module;
   vkCreateShaderModule(vc->device,
                        &(VkShaderModuleCreateInfo) {
                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                           .codeSize = sizeof(fs_source),
                           .pCode = fs_source,
                        },
                        &fs_module);

   VkShader fs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .module = fs_module,
                     .pName = "main"
                  },
                  &fs);

   vkCreateGraphicsPipelines(vc->device,
      (VkPipelineCache) { VK_NULL_HANDLE },
      1,
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
             {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX,
                .shader = vs
             },
             {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT,
                .shader = fs,
             },
         },
         .pVertexInputState = &vi_create_info,
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },

         .pViewportState = &(VkPipelineViewportStateCreateInfo) {},
    
         .pRasterState = &(VkPipelineRasterStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO,
            .depthClipEnable = true,
            .rasterizerDiscardEnable = false,
            .fillMode = VK_FILL_MODE_SOLID,
            .cullMode = VK_CULL_MODE_BACK,
            .frontFace = VK_FRONT_FACE_CW
         },

         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {},
         .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo) {},

         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = (VkPipelineColorBlendAttachmentState []) {
               { .channelWriteMask = VK_CHANNEL_A_BIT |
                 VK_CHANNEL_R_BIT | VK_CHANNEL_G_BIT | VK_CHANNEL_B_BIT },
            }
         },

         .flags = 0,
         .layout = vc->pipeline_layout,
         .renderPass = vc->render_pass,
         .subpass = 0,
         .basePipelineHandle = (VkPipeline) { 0 },
         .basePipelineIndex = 0
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
                    .memoryTypeIndex = 0,
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

   vkBindBufferMemory(vc->device, vc->buffer, vc->mem, 0);

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
                                &(VkDynamicViewportStateCreateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_DYNAMIC_VIEWPORT_STATE_CREATE_INFO,
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
                                   .pScissors = (VkRect2D[]) {
                                      { {  0,  0 }, { vc->width, vc->height } },
                                   }
                                },
                                &vc->vp_state);

   vkCreateDynamicRasterState(vc->device,
                              &(VkDynamicRasterStateCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_DYNAMIC_RASTER_STATE_CREATE_INFO,
                              },
                              &vc->rs_state);   

   vkCreateDynamicColorBlendState(vc->device,
                                  &(VkDynamicColorBlendStateCreateInfo) {
                                     .sType = VK_STRUCTURE_TYPE_DYNAMIC_COLOR_BLEND_STATE_CREATE_INFO
                                  },
                                  &vc->cb_state);

   vkAllocDescriptorSets(vc->device, (VkDescriptorPool) { 0 },
                         VK_DESCRIPTOR_SET_USAGE_STATIC,
                         1, &set_layout, &vc->descriptor_set, &count);

   vkUpdateDescriptorSets(vc->device, 1,
                          (VkWriteDescriptorSet []) {
                             {
                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .destSet = vc->descriptor_set,
                                .destBinding = 0,
                                .destArrayElement = 0,
                                .count = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                .pDescriptors = (VkDescriptorInfo []) {
                                   {
                                      .bufferView = vc->ubo_view,
                                   }
                                }
                             }
                          },
                          0, NULL);

   vkCreateFence(vc->device,
                 &(VkFenceCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                    .flags = 0
                 },
                 &vc->fence);

   vkCreateCommandPool(vc->device,
                       &(const VkCmdPoolCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_CMD_POOL_CREATE_INFO,
                          .queueFamilyIndex = 0,
                          .flags = 0
                       },
                       &vc->cmd_pool);
}

static void
init_buffer(struct vkcube *vc, struct vkcube_buffer *b)
{
   vkCreateAttachmentView(vc->device,
                          &(VkAttachmentViewCreateInfo) {
                             .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
                             .image = b->image,
                             .format = VK_FORMAT_B8G8R8A8_UNORM,
                             .mipLevel = 0,
                             .baseArraySlice = 0,
                             .arraySize = 1,
                          },
                          &b->view);
   
   vkCreateFramebuffer(vc->device,
                       &(VkFramebufferCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                          .attachmentCount = 1,
                          .pAttachments = (VkAttachmentBindInfo[]) {
                             {
                                .view = b->view,
                                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                             }
                          },
                          .width = vc->width,
                          .height = vc->height,
                          .layers = 1
                       },
                       &b->framebuffer);
}

static void
render_cube_frame(struct vkcube *vc, struct vkcube_buffer *b)
{
   struct ubo ubo;
   struct timeval tv;
   uint64_t t;

   gettimeofday(&tv, NULL);

   t = ((tv.tv_sec * 1000 + tv.tv_usec / 1000) -
        (vc->start_tv.tv_sec * 1000 + vc->start_tv.tv_usec / 1000)) / 5;

   esMatrixLoadIdentity(&ubo.modelview);
   esTranslate(&ubo.modelview, 0.0f, 0.0f, -8.0f);
   esRotate(&ubo.modelview, 45.0f + (0.25f * t), 1.0f, 0.0f, 0.0f);
   esRotate(&ubo.modelview, 45.0f - (0.5f * t), 0.0f, 1.0f, 0.0f);
   esRotate(&ubo.modelview, 10.0f + (0.15f * t), 0.0f, 0.0f, 1.0f);

   float aspect = (float) vc->height / (float) vc->width;
   ESMatrix projection;
   esMatrixLoadIdentity(&projection);
   esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 6.0f, 10.0f);

   esMatrixLoadIdentity(&ubo.modelviewprojection);
   esMatrixMultiply(&ubo.modelviewprojection, &ubo.modelview, &projection);

   /* The mat3 normalMatrix is laid out as 3 vec4s. */
   memcpy(ubo.normal, &ubo.modelview, sizeof ubo.normal);

   memcpy(vc->map, &ubo, sizeof(ubo));

   VkCmdBuffer cmd_buffer;
   vkCreateCommandBuffer(vc->device,
                         &(VkCmdBufferCreateInfo) {
                            .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_CREATE_INFO,
                            .cmdPool = vc->cmd_pool,
                            .level = 0,
                         },
                         &cmd_buffer);

   vkBeginCommandBuffer(cmd_buffer,
                        &(VkCmdBufferBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO,
                           .flags = 0
                        });

   vkCmdBeginRenderPass(cmd_buffer,
                        &(VkRenderPassBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                           .renderPass = vc->render_pass,
                           .framebuffer = b->framebuffer,
                           .renderArea = { { 0, 0 }, { vc->width, vc->height } },
                           .attachmentCount = 1,
                           .pAttachmentClearValues = (VkClearValue []) {
                              { .color = { .f32 = { 0.2f, 0.2f, 0.2f, 1.0f } } }
                           }
                        },
                        VK_RENDER_PASS_CONTENTS_INLINE);

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
                           VK_PIPELINE_BIND_POINT_GRAPHICS,
                           vc->pipeline_layout,
                           0, 1,
                           &vc->descriptor_set, 0, NULL);

   vkCmdBindDynamicViewportState(cmd_buffer, vc->vp_state);
   vkCmdBindDynamicRasterState(cmd_buffer, vc->rs_state);
   vkCmdBindDynamicColorBlendState(cmd_buffer, vc->cb_state);

   vkCmdDraw(cmd_buffer, 0, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 4, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 8, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 12, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 16, 4, 0, 1);
   vkCmdDraw(cmd_buffer, 20, 4, 0, 1);

   vkCmdEndRenderPass(cmd_buffer);

   vkEndCommandBuffer(cmd_buffer);

   vkQueueSubmit(vc->queue, 1, &cmd_buffer, vc->fence);

   vkWaitForFences(vc->device, 1, (VkFence[]) { vc->fence }, true, INT64_MAX);

   vkResetCommandPool(vc->device, vc->cmd_pool, 0);
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
   init_vk(vc);

   struct vkcube_buffer *b = &vc->buffers[0];

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
   vkGetImageMemoryRequirements(vc->device, b->image, &requirements);

   vkAllocMemory(vc->device,
                 &(VkMemoryAllocInfo) {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                    .allocationSize = requirements.size,
                    .memoryTypeIndex = 0
                 },
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

   init_vk(vc);

   for (uint32_t i = 0; i < 2; i++) {
      struct vkcube_buffer *b = &vc->buffers[i];
      int fd, stride, ret;

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
         render_cube_frame(vc, b);

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

   init_vk(vc);

   VkSurfaceDescriptionWindowWSI vk_window = {
      .sType = VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI,
      .platform = VK_PLATFORM_XCB_WSI,
      .pPlatformHandle = &(VkPlatformHandleXcbWSI) {
         .connection = vc->xcb.conn,
         .root = iter.data->root,
      },
      .pPlatformWindow = (void*) (intptr_t) vc->xcb.window,
   };

   VkBool32 supported;
   vkGetPhysicalDeviceSurfaceSupportWSI(vc->physical_device, 0,
                                        (VkSurfaceDescriptionWSI *)&vk_window,
                                        &supported);
   if (!supported) {
      fprintf(stderr, "Vulkan not supported on given X window");
      abort();
   }

   vkCreateSwapChainWSI(vc->device,
                        &(VkSwapChainCreateInfoWSI) {
                           .sType = VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI,
                           .pSurfaceDescription = (VkSurfaceDescriptionWSI *)&vk_window,
                           .minImageCount = 2,
                           .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                           .imageExtent = { vc->width, vc->height },
                           .imageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           .preTransform = VK_SURFACE_TRANSFORM_NONE_WSI,
                           .imageArraySize = 1,
                           .presentMode = VK_PRESENT_MODE_MAILBOX_WSI,
                        }, &vc->swap_chain);

   size_t size = 0;
   vkGetSwapChainInfoWSI(vc->device, vc->swap_chain,
                         VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI,
                         &size, NULL);
   assert(size > 0);
   const int image_count = size / sizeof(VkSwapChainImagePropertiesWSI);

   VkSwapChainImagePropertiesWSI swap_chain_images[image_count];
   vkGetSwapChainInfoWSI(vc->device, vc->swap_chain,
                         VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI,
                         &size, swap_chain_images);

   for (uint32_t i = 0; i < image_count; i++) {
      vc->buffers[i].image = swap_chain_images[i].image;
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

   while (1) {
      event = xcb_wait_for_event(vc->xcb.conn);
      switch (event->response_type & 0x7f) {
      case XCB_CLIENT_MESSAGE:
         client_message = (xcb_client_message_event_t *) event;
         if (client_message->window != vc->xcb.window)
            break;

         if (client_message->type == vc->xcb.atom_wm_protocols &&
             client_message->data.data32[0] == vc->xcb.atom_wm_delete_window) {
            exit(0);
         }

         if (client_message->type == XCB_ATOM_NOTICE) {
            uint32_t index;
            vkAcquireNextImageWSI(vc->device, vc->swap_chain, 60,
                                  (VkSemaphore) { 0 }, &index);

            render_cube_frame(vc, &vc->buffers[index]);

            vkQueuePresentWSI(vc->queue,
                              &(VkPresentInfoWSI) {
                                 .sType = VK_STRUCTURE_TYPE_QUEUE_PRESENT_INFO_WSI,
                                 .swapChainCount = 1,
                                 .swapChains = (VkSwapChainWSI[]) {
                                    vc->swap_chain,
                                 },
                                 .imageIndices = (uint32_t[]) {
                                    index,
                                 },
                              });

            schedule_xcb_repaint(vc);
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

   vc->wl.xdg_surface = xdg_shell_get_xdg_surface(vc->wl.shell,
                                                  vc->wl.surface);
   xdg_surface_add_listener(vc->wl.xdg_surface, &xdg_surface_listener, vc);
   xdg_surface_set_title(vc->wl.xdg_surface, "vkcube");

   init_vk(vc);

   VkSurfaceDescriptionWindowWSI vk_window = {
      .sType = VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI,
      .platform = VK_PLATFORM_WAYLAND_WSI,
      .pPlatformHandle = vc->wl.display,
      .pPlatformWindow = vc->wl.surface,
   };

   VkBool32 supported;
   vkGetPhysicalDeviceSurfaceSupportWSI(vc->physical_device, 0,
                                        (VkSurfaceDescriptionWSI *)&vk_window,
                                        &supported);
   if (!supported) {
      fprintf(stderr, "Vulkan not supported on given Wayland surface");
      abort();
   }

   size_t size = 0;
   vkGetSurfaceInfoWSI(vc->device, (VkSurfaceDescriptionWSI *)&vk_window,
                       VK_SURFACE_INFO_TYPE_FORMATS_WSI, &size, NULL);
   assert(size > 0);

   const int num_formats = size / sizeof(VkSurfaceFormatPropertiesWSI);
   VkSurfaceFormatPropertiesWSI formats[num_formats];

   vkGetSurfaceInfoWSI(vc->device, (VkSurfaceDescriptionWSI *)&vk_window,
                       VK_SURFACE_INFO_TYPE_FORMATS_WSI, &size, formats);

   vkGetPhysicalDeviceSurfaceSupportWSI(vc->physical_device, 0,
                                        (VkSurfaceDescriptionWSI *)&vk_window,
                                        &supported);

   VkFormat format = VK_FORMAT_UNDEFINED;
   for (int i = 0; i < num_formats; i++) {
      switch (formats[i].format) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_UNORM:
         /* These formats are all fine */
         format = formats[i].format;
         break;
      case VK_FORMAT_R8G8B8_UNORM:
      case VK_FORMAT_B8G8R8_UNORM:
      case VK_FORMAT_R5G6B5_UNORM:
      case VK_FORMAT_B5G6R5_UNORM:
         /* We would like to support these but they don't seem to work. */
      default:
         continue;
      }
   }

   assert(format != VK_FORMAT_UNDEFINED);

   vkCreateSwapChainWSI(vc->device,
                        &(VkSwapChainCreateInfoWSI) {
                           .sType = VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI,
                           .pSurfaceDescription = (VkSurfaceDescriptionWSI *)&vk_window,
                           .minImageCount = 2,
                           .imageFormat = format,
                           .imageExtent = { vc->width, vc->height },
                           .imageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           .preTransform = VK_SURFACE_TRANSFORM_NONE_WSI,
                           .imageArraySize = 1,
                           .presentMode = VK_PRESENT_MODE_MAILBOX_WSI,
                        }, &vc->swap_chain);

   size = 0;
   vkGetSwapChainInfoWSI(vc->device, vc->swap_chain,
                         VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI,
                         &size, NULL);
   assert(size > 0);
   const int image_count = size / sizeof(VkSwapChainImagePropertiesWSI);

   VkSwapChainImagePropertiesWSI swap_chain_images[image_count];
   vkGetSwapChainInfoWSI(vc->device, vc->swap_chain,
                         VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI,
                         &size, swap_chain_images);

   for (uint32_t i = 0; i < image_count; i++) {
      vc->buffers[i].image = swap_chain_images[i].image;
      init_buffer(vc, &vc->buffers[i]);
   }
}

static void
mainloop_wayland(struct vkcube *vc)
{
   VkResult result = VK_SUCCESS;
   while (1) {
      uint32_t index;
      result = vkAcquireNextImageWSI(vc->device, vc->swap_chain, 60,
                                     (VkSemaphore) { 0 }, &index);
      if (result != VK_SUCCESS)
         return;

      render_cube_frame(vc, &vc->buffers[index]);

      vkQueuePresentWSI(vc->queue,
                        &(VkPresentInfoWSI) {
                           .sType = VK_STRUCTURE_TYPE_QUEUE_PRESENT_INFO_WSI,
                           .swapChainCount = 1,
                           .swapChains = (VkSwapChainWSI[]) {
                              vc->swap_chain,
                           },
                           .imageIndices = (uint32_t[]) {
                              index,
                           },
                        });
      if (result != VK_SUCCESS)
         return;
   }
}

int main(int argc, char *argv[])
{
   struct vkcube vc;
   bool headless;

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
      render_cube_frame(&vc, &vc.buffers[0]);
      write_buffer(&vc, &vc.buffers[0]);
   }

   return 0;
}
