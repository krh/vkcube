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

#include "common.h"

struct ubo {
   ESMatrix modelview;
   ESMatrix modelviewprojection;
   float normal[12];
};

static char vs_spirv_source[] = {
#include "vkcube.vert.spv.h"
};

static char fs_spirv_source[] = {
#include "vkcube.frag.spv.h"
};

static void
init_cube(struct vkcube *vc)
{
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

   VkShaderModule vs_module;
   vkCreateShaderModule(vc->device,
                        &(VkShaderModuleCreateInfo) {
                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                           .codeSize = sizeof(vs_spirv_source),
                           .pCode = vs_spirv_source,
                        },
                        &vs_module);

   VkShader vs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .module = vs_module,
                     .pName = "main",
                     .stage = VK_SHADER_STAGE_VERTEX,
                  },
                  &vs);

   VkShaderModule fs_module;
   vkCreateShaderModule(vc->device,
                        &(VkShaderModuleCreateInfo) {
                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                           .codeSize = sizeof(fs_spirv_source),
                           .pCode = fs_spirv_source,
                        },
                        &fs_module);

   VkShader fs;
   vkCreateShader(vc->device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .module = fs_module,
                     .pName = "main",
                     .stage = VK_SHADER_STAGE_FRAGMENT,
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
                         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                         .offset = 0,
                         .range = sizeof(struct ubo)
                      },
                      &vc->ubo_view);

   vkAllocDescriptorSets(vc->device, (VkDescriptorPool) { 0 },
                         VK_DESCRIPTOR_SET_USAGE_STATIC,
                         1, &set_layout, &vc->descriptor_set);

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
}

static void
render_cube(struct vkcube *vc, struct vkcube_buffer *b)
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
                           .clearValueCount = 1,
                           .pClearValues = (VkClearValue []) {
                              { .color = { .float32 = { 0.2f, 0.2f, 0.2f, 1.0f } } }
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

   vkCmdDraw(cmd_buffer, 4, 1, 0, 0);
   vkCmdDraw(cmd_buffer, 4, 1, 4, 0);
   vkCmdDraw(cmd_buffer, 4, 1, 8, 0);
   vkCmdDraw(cmd_buffer, 4, 1, 12, 0);
   vkCmdDraw(cmd_buffer, 4, 1, 16, 0);
   vkCmdDraw(cmd_buffer, 4, 1, 20, 0);

   vkCmdEndRenderPass(cmd_buffer);

   vkEndCommandBuffer(cmd_buffer);

   vkQueueSubmit(vc->queue, 1, &cmd_buffer, vc->fence);

   vkWaitForFences(vc->device, 1, (VkFence[]) { vc->fence }, true, INT64_MAX);

   vkResetCommandPool(vc->device, vc->cmd_pool, 0);
}

struct model cube_model = {
   .init = init_cube,
   .render = render_cube
};
