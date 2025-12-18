#include "../../../include/encoder/video/vulkan.h"
#include "../../../include/utils.h"
#include "../../../include/egl.h"

#include <libavcodec/avcodec.h>
#define VK_NO_PROTOTYPES
//#include <libavutil/hwcontext_vulkan.h>

//#include <vulkan/vulkan_core.h>

#define GL_HANDLE_TYPE_OPAQUE_FD_EXT      0x9586
#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_OPTIMAL_TILING_EXT             0x9584
#define GL_LINEAR_TILING_EXT              0x9585

typedef struct {
    gsr_video_encoder_vulkan_params params;
    unsigned int target_textures[2];
    vec2i texture_sizes[2];
    AVBufferRef *device_ctx;
} gsr_video_encoder_vulkan;

static bool gsr_video_encoder_vulkan_setup_context(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context) {
    AVDictionary *options = NULL;
    //av_dict_set(&options, "linear_images", "1", 0);
    //av_dict_set(&options, "disable_multiplane", "1", 0);
#if 0
    // TODO: Use correct device
    if(av_hwdevice_ctx_create(&self->device_ctx, AV_HWDEVICE_TYPE_VULKAN, NULL, options, 0) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(self->device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to create hwframe context\n");
        av_buffer_unref(&self->device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)self->device_ctx->data;

    //AVVulkanFramesContext *vk_frame_ctx = (AVVulkanFramesContext*)hw_frame_context->hwctx;
    //hw_frame_context->initial_pool_size = 20;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_context: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&self->device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    av_buffer_unref(&frame_context);
#endif
    return true;
}
#if 0
static AVVulkanDeviceContext* video_codec_context_get_vulkan_data(AVCodecContext *video_codec_context) {
    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return NULL;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    if(device_context->type != AV_HWDEVICE_TYPE_VULKAN)
        return NULL;

    return (AVVulkanDeviceContext*)device_context->hwctx;
}

static uint32_t get_memory_type_idx(VkPhysicalDevice pdev, const VkMemoryRequirements *mem_reqs, VkMemoryPropertyFlagBits prop_flags, PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties) {
    VkPhysicalDeviceMemoryProperties pdev_mem_props;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(pdev, &pdev_mem_props);

    for (i = 0; i < pdev_mem_props.memoryTypeCount; i++) {
        const VkMemoryType *type = &pdev_mem_props.memoryTypes[i];

        if ((mem_reqs->memoryTypeBits & (1 << i)) &&
            (type->propertyFlags & prop_flags) == prop_flags) {
            return i;
            break;
        }
    }
    return UINT32_MAX;
}
#endif
static bool gsr_video_encoder_vulkan_setup_textures(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    const int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vulkan_setup_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    while(self->params.egl->glGetError()) {}
#if 0
    AVVkFrame *target_surface_id = (AVVkFrame*)frame->data[0];
    AVVulkanDeviceContext* vv = video_codec_context_get_vulkan_data(video_codec_context);
    const size_t luma_size = frame->width * frame->height;
    if(vv) {
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)vv->get_proc_addr(vv->inst, "vkGetImageMemoryRequirements");
        PFN_vkAllocateMemory vkAllocateMemory = (PFN_vkAllocateMemory)vv->get_proc_addr(vv->inst, "vkAllocateMemory");
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)vv->get_proc_addr(vv->inst, "vkGetPhysicalDeviceMemoryProperties");
        PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vv->get_proc_addr(vv->inst, "vkGetMemoryFdKHR");

        VkMemoryRequirements mem_reqs = {0};
        vkGetImageMemoryRequirements(vv->act_dev, target_surface_id->img[0], &mem_reqs);

        fprintf(stderr, "size: %lu, alignment: %lu, memory bits: 0x%08x\n", mem_reqs.size, mem_reqs.alignment, mem_reqs.memoryTypeBits);
        VkDeviceMemory mem;
        {
            VkExportMemoryAllocateInfo exp_mem_info;
            VkMemoryAllocateInfo mem_alloc_info;
            VkMemoryDedicatedAllocateInfoKHR ded_info;

            memset(&exp_mem_info, 0, sizeof(exp_mem_info));
            exp_mem_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exp_mem_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            
            memset(&ded_info, 0, sizeof(ded_info));
            ded_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            ded_info.image = target_surface_id->img[0];

            exp_mem_info.pNext = &ded_info;

            memset(&mem_alloc_info, 0, sizeof(mem_alloc_info));
            mem_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mem_alloc_info.pNext = &exp_mem_info;
            mem_alloc_info.allocationSize = target_surface_id->size[0];
            mem_alloc_info.memoryTypeIndex = get_memory_type_idx(vv->phys_dev, &mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vkGetPhysicalDeviceMemoryProperties);

            if (mem_alloc_info.memoryTypeIndex == UINT32_MAX) {
                fprintf(stderr, "No suitable memory type index found.\n");
                return VK_NULL_HANDLE;
            }

            if (vkAllocateMemory(vv->act_dev, &mem_alloc_info, 0, &mem) !=
                VK_SUCCESS)
                return VK_NULL_HANDLE;
 
            fprintf(stderr, "memory: %p\n", (void*)mem);

        }

        fprintf(stderr, "target surface id: %p, %zu, %zu\n", (void*)target_surface_id->mem[0], target_surface_id->offset[0], target_surface_id->offset[1]);
        fprintf(stderr, "vkGetMemoryFdKHR: %p\n", (void*)vkGetMemoryFdKHR);

        int fd = 0;
        VkMemoryGetFdInfoKHR fd_info;
        memset(&fd_info, 0, sizeof(fd_info));
        fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fd_info.memory = target_surface_id->mem[0];
        fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if(vkGetMemoryFdKHR(vv->act_dev, &fd_info, &fd) != VK_SUCCESS) {
            fprintf(stderr, "failed!\n");
        } else {
            fprintf(stderr, "fd: %d\n", fd);
        }

        fprintf(stderr, "glImportMemoryFdEXT: %p, size: %zu\n", (void*)self->params.egl->glImportMemoryFdEXT, target_surface_id->size[0]);
        const int tiling = target_surface_id->tiling == VK_IMAGE_TILING_LINEAR ? GL_LINEAR_TILING_EXT : GL_OPTIMAL_TILING_EXT;

        if(tiling != GL_OPTIMAL_TILING_EXT) {
            fprintf(stderr, "tiling %d is not supported, only GL_OPTIMAL_TILING_EXT (%d) is supported\n", tiling, GL_OPTIMAL_TILING_EXT);
        }


        unsigned int gl_memory_obj = 0;
        self->params.egl->glCreateMemoryObjectsEXT(1, &gl_memory_obj);

        //const int dedicated = GL_TRUE;
        //self->params.egl->glMemoryObjectParameterivEXT(gl_memory_obj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);

        self->params.egl->glImportMemoryFdEXT(gl_memory_obj, target_surface_id->size[0], GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        if(!self->params.egl->glIsMemoryObjectEXT(gl_memory_obj))
            fprintf(stderr, "failed to create object!\n");

        fprintf(stderr, "gl memory obj: %u, error: %d\n", gl_memory_obj, self->params.egl->glGetError());

        // fprintf(stderr, "0 gl error: %d\n", self->params.egl->glGetError());
        // unsigned int vertex_buffer = 0;
        // self->params.egl->glGenBuffers(1, &vertex_buffer);
        // self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        // self->params.egl->glBufferStorageMemEXT(GL_ARRAY_BUFFER, target_surface_id->size[0], gl_memory_obj, target_surface_id->offset[0]);
        // fprintf(stderr, "1 gl error: %d\n", self->params.egl->glGetError());

        // fprintf(stderr, "0 gl error: %d\n", self->params.egl->glGetError());
        // unsigned int buffer = 0;
        // self->params.egl->glCreateBuffers(1, &buffer);
        // self->params.egl->glNamedBufferStorageMemEXT(buffer, target_surface_id->size[0], gl_memory_obj, target_surface_id->offset[0]);
        // fprintf(stderr, "1 gl error: %d\n", self->params.egl->glGetError());

        self->params.egl->glGenTextures(1, &self->target_textures[0]);
        self->params.egl->glBindTexture(GL_TEXTURE_2D, self->target_textures[0]);

        fprintf(stderr, "1 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, tiling);

        fprintf(stderr, "tiling: %d\n", tiling);

        fprintf(stderr, "2 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_R8, frame->width, frame->height, gl_memory_obj, target_surface_id->offset[0]);

        fprintf(stderr, "3 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        self->params.egl->glGenTextures(1, &self->target_textures[1]);
        self->params.egl->glBindTexture(GL_TEXTURE_2D, self->target_textures[1]);

        fprintf(stderr, "1 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, tiling);

        fprintf(stderr, "tiling: %d\n", tiling);

        fprintf(stderr, "2 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RG8, frame->width/2, frame->height/2, gl_memory_obj, target_surface_id->offset[0] + luma_size);

        fprintf(stderr, "3 gl error: %d\n", self->params.egl->glGetError());
        self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        self->texture_sizes[0] = (vec2i){ frame->width, frame->height };
        self->texture_sizes[1] = (vec2i){ frame->width/2, frame->height/2 };
     }
#endif
    return true;
}

static void gsr_video_encoder_vulkan_stop(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context);

static bool gsr_video_encoder_vulkan_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_video_encoder_vulkan *self = encoder->priv;

    video_codec_context->width = FFALIGN(video_codec_context->width, 2);
    video_codec_context->height = FFALIGN(video_codec_context->height, 2);

    if(video_codec_context->width < 128)
        video_codec_context->width = 128;

    if(video_codec_context->height < 128)
        video_codec_context->height = 128;

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(!gsr_video_encoder_vulkan_setup_context(self, video_codec_context)) {
        gsr_video_encoder_vulkan_stop(self, video_codec_context);
        return false;
    }

    if(!gsr_video_encoder_vulkan_setup_textures(self, video_codec_context, frame)) {
        gsr_video_encoder_vulkan_stop(self, video_codec_context);
        return false;
    }

    return true;
}

void gsr_video_encoder_vulkan_stop(gsr_video_encoder_vulkan *self, AVCodecContext *video_codec_context) {
    self->params.egl->glDeleteTextures(2, self->target_textures);
    self->target_textures[0] = 0;
    self->target_textures[1] = 0;

    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);
    if(self->device_ctx)
        av_buffer_unref(&self->device_ctx);
}

static void gsr_video_encoder_vulkan_get_textures(gsr_video_encoder *encoder, unsigned int *textures, vec2i *texture_sizes, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_vulkan *self = encoder->priv;
    textures[0] = self->target_textures[0];
    textures[1] = self->target_textures[1];
    texture_sizes[0] = self->texture_sizes[0];
    texture_sizes[1] = self->texture_sizes[1];
    *num_textures = 2;
    *destination_color = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
}

static void gsr_video_encoder_vulkan_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    gsr_video_encoder_vulkan_stop(encoder->priv, video_codec_context);
    free(encoder->priv);
    free(encoder);
}

gsr_video_encoder* gsr_video_encoder_vulkan_create(const gsr_video_encoder_vulkan_params *params) {
    gsr_video_encoder *encoder = calloc(1, sizeof(gsr_video_encoder));
    if(!encoder)
        return NULL;

    gsr_video_encoder_vulkan *encoder_vulkan = calloc(1, sizeof(gsr_video_encoder_vulkan));
    if(!encoder_vulkan) {
        free(encoder);
        return NULL;
    }

    encoder_vulkan->params = *params;

    *encoder = (gsr_video_encoder) {
        .start = gsr_video_encoder_vulkan_start,
        .copy_textures_to_frame = NULL,
        .get_textures = gsr_video_encoder_vulkan_get_textures,
        .destroy = gsr_video_encoder_vulkan_destroy,
        .priv = encoder_vulkan
    };

    return encoder;
}
