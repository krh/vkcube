#include <stdarg.h>
#include <stdnoreturn.h>
#include <sys/time.h>
#include <stdbool.h>
#include <string.h>

#if defined(HAVE_VULKAN_INTEL_H)
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#endif

#include <png.h>

#if defined(ENABLE_XCB)
#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#endif

#if defined(ENABLE_WAYLAND)
#include <wayland-client.h>
#include <xdg-shell-protocol.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>

#include "esUtil.h"

#define printflike(a, b) __attribute__((format(printf, (a), (b))))

#define MAX_NUM_IMAGES 5

struct vkcube_buffer {
#if defined(HAVE_VULKAN_INTEL_H)
   struct gbm_bo *gbm_bo;
   uint32_t fb;
#endif
   VkDeviceMemory mem;
   VkImage image;
   VkImageView view;
   VkFramebuffer framebuffer;
   VkFence fence;
   VkCommandBuffer cmd_buffer;

   uint32_t stride;
};

struct vkcube;

struct model {
   void (*init)(struct vkcube *vc);
   void (*render)(struct vkcube *vc, struct vkcube_buffer *b, bool wait_semaphore);
};

struct vkcube {
   struct model model;

   bool protected;

#if defined(HAVE_VULKAN_INTEL_H)
   int fd;
   drmModeCrtc *crtc;
   drmModeConnector *connector;
   struct gbm_device *gbm_device;
#endif

#if defined(ENABLE_XCB)
   struct {
      xcb_connection_t *conn;
      xcb_window_t window;
      xcb_atom_t atom_wm_protocols;
      xcb_atom_t atom_wm_delete_window;
   } xcb;
#endif

#if defined(ENABLE_WAYLAND)
   struct {
      struct wl_display *display;
      struct wl_compositor *compositor;
      struct xdg_wm_base *shell;
      struct wl_keyboard *keyboard;
      struct wl_seat *seat;
      struct wl_surface *surface;
      struct xdg_surface *xdg_surface;
      struct xdg_toplevel *xdg_toplevel;
      bool wait_for_configure;
   } wl;
#endif

   struct {
      VkDisplayModeKHR display_mode;
   } khr;

   VkSwapchainKHR swap_chain;

   uint32_t width, height;

   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkDevice device;
   VkRenderPass render_pass;
   VkQueue queue;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkDeviceMemory mem;
   VkBuffer buffer;
   VkDescriptorSet descriptor_set;
   VkSemaphore semaphore;
   VkCommandPool cmd_pool;

   void *map;
   uint32_t vertex_offset, colors_offset, normals_offset;

   struct timeval start_tv;
   VkSurfaceKHR surface;
   VkFormat image_format;
   struct vkcube_buffer buffers[MAX_NUM_IMAGES];
   uint32_t image_count;
   int current;
};

void noreturn failv(const char *format, va_list args);
void noreturn fail(const char *format, ...) printflike(1, 2) ;
void fail_if(int cond, const char *format, ...) printflike(2, 3);

static inline bool
streq(const char *a, const char *b)
{
   return strcmp(a, b) == 0;
}
