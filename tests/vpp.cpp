/*
 * Copyright (C) 2011-2014 Intel Corporation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vppinputoutput.h"
#include "vppoutputencode.h"
#include "encodeinput.h"
#include "common/log.h"
#include <VideoEncoderInterface.h>
#include <VideoEncoderHost.h>
#include <VideoPostProcessHost.h>
#include <YamiVersion.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

using namespace YamiMediaCodec;

void usage();

SharedPtr<VppInput> createInput(const char* filename, const SharedPtr<VADisplay>& display)
{
    SharedPtr<VppInput> input(VppInput::create(filename));
    if (!input) {
        ERROR("creat input failed");
        return input;
    }
    SharedPtr<VppInputFile> inputFile = std::tr1::dynamic_pointer_cast<VppInputFile>(input);
    if (inputFile) {
        SharedPtr<FrameReader> reader(new VaapiFrameReader(display));
        SharedPtr<FrameAllocator> alloctor(new PooledFrameAllocator(display, 5));
        inputFile->config(alloctor, reader);
    }
    return inputFile;
}

SharedPtr<VppOutput> createOutput(const char* filename, const SharedPtr<VADisplay>& display)
{

    SharedPtr<VppOutput> output = VppOutput::create(filename);
    SharedPtr<VppOutputFile> outputFile = std::tr1::dynamic_pointer_cast<VppOutputFile>(output);
    if (outputFile) {
        SharedPtr<FrameWriter> writer(new VaapiFrameWriter(display));
        if (!outputFile->config(writer)) {
            ERROR("config writer failed");
            output.reset();
        }
        return output;
    }
    SharedPtr<VppOutputEncode> outputEncode = std::tr1::dynamic_pointer_cast<VppOutputEncode>(output);
    if (outputEncode) {
        NativeDisplay nativeDisplay;
        nativeDisplay.type = NATIVE_DISPLAY_VA;
        nativeDisplay.handle = (intptr_t)*display;
        if (!outputEncode->config(nativeDisplay)) {
            ERROR("config ouput encode failed");
            output.reset();
        }
        return output;
    }
    return output;
}

SharedPtr<FrameAllocator> createAllocator(const SharedPtr<VppOutput>& output, const SharedPtr<VADisplay>& display)
{
    uint32_t fourcc;
    int width, height;
    SharedPtr<FrameAllocator> allocator(new PooledFrameAllocator(display, 5));
    if (!output->getFormat(fourcc, width, height)
        || !allocator->setFormat(fourcc, width,height)) {
        allocator.reset();
        ERROR("get Format failed");
    }
    return allocator;
}

class VppTest
{
public:
    VppTest()
#if YAMI_CHECK_API_VERSION(0, 2, 1)
        : m_sharpening(SHARPENING_LEVEL_NONE)
        , m_denoise(DENOISE_LEVEL_NONE)
        , m_deinterlaceMode(NULL)
#endif
    {
    }
    bool init(int argc, char** argv)
    {
        if (!processCmdLine(argc, argv))
            return false;
        m_display = createVADisplay();
        if (!m_display) {
            printf("create display failed");
            return false;
        }
        if (!createVpp()) {
            ERROR("create vpp failed");
            return false;
        }
        m_input = createInput(m_inputName, m_display);
        m_output = createOutput(m_outputName, m_display);
        if (!m_input || !m_output) {
            printf("create input or output failed");
            return false;
        }
        m_allocator = createAllocator(m_output, m_display);
        return m_allocator;
    }

    bool run()
    {

        SharedPtr<VideoFrame> src, dest;
        YamiStatus  status;
        int count = 0;
        while (m_input->read(src)) {
            dest = m_allocator->alloc();
            status = m_vpp->process(src, dest);
            if (status != YAMI_SUCCESS) {
                ERROR("vpp process failed, status = %d", status);
                return true;
            }
            m_output->output(dest);
            count++;
        }
        //flush output
        dest.reset();
        m_output->output(dest);

        printf("%d frame processed\n", count);
        return true;
    }
private:
    bool processCmdLine(int argc, char* argv[])
    {
        char opt;
        const struct option long_opts[] = {
            { "help", no_argument, NULL, 'h' },
            { "sharpening", required_argument, NULL, 's' },
            { "dn", required_argument, NULL, 0 },
            { "di", required_argument, NULL, 0 },
            { NULL, no_argument, NULL, 0 }
        };
        int option_index;

        if (argc < 3) {
            usage();
            return false;
        }

        while ((opt = getopt_long_only(argc, argv, "s:h:", long_opts, &option_index)) != -1) {
            switch (opt) {
            case 'h':
            case '?':
                usage();
                return false;
            case 's':
                m_sharpening = atoi(optarg);
                break;
            case 0:
                switch (option_index) {
                case 2:
                    m_denoise = atoi(optarg);
                    break;
                case 3:
                    m_deinterlaceMode = optarg;
                    break;
                default:
                    usage();
                    return false;
                }
                break;
            default:
                usage();
                return false;
            }
        }
        if (optind + 2 < argc) {
            usage();
            return false;
        }
        m_inputName = argv[optind++];
        m_outputName = argv[optind++];
        return true;
    }

    bool createVpp()
    {
        NativeDisplay nativeDisplay;
        nativeDisplay.type = NATIVE_DISPLAY_VA;
        nativeDisplay.handle = (intptr_t)*m_display;
        m_vpp.reset(createVideoPostProcess(YAMI_VPP_SCALER), releaseVideoPostProcess);
        if (m_vpp->setNativeDisplay(nativeDisplay) != YAMI_SUCCESS)
            return false;
#if YAMI_CHECK_API_VERSION(0, 2, 1)
        {
            VPPDenoiseParameters denoise;
            memset(&denoise, 0, sizeof(denoise));
            denoise.size = sizeof(denoise);
            denoise.level = m_denoise;
            if (m_vpp->setParameters(VppParamTypeDenoise, &denoise) != YAMI_SUCCESS) {
                ERROR("denoise level should in range [%d, %d] or %d for none",
                    DENOISE_LEVEL_MIN, DENOISE_LEVEL_MAX, DENOISE_LEVEL_NONE);
                return false;
            }
        }

        {
            VPPSharpeningParameters sharpening;
            memset(&sharpening, 0, sizeof(sharpening));
            sharpening.size = sizeof(sharpening);
            sharpening.level = m_sharpening;
            if (m_vpp->setParameters(VppParamTypeSharpening, &sharpening) != YAMI_SUCCESS) {
                ERROR("sharpening level should in range [%d, %d] or %d for none",
                    SHARPENING_LEVEL_MIN, SHARPENING_LEVEL_MAX, SHARPENING_LEVEL_NONE);
                return false;
            }
        }
        if (m_deinterlaceMode) {
            VPPDeinterlaceParameters deinterlace;
            memset(&deinterlace, 0, sizeof(deinterlace));
            deinterlace.size = sizeof(deinterlace);
            if (strcasecmp(m_deinterlaceMode, "bob") == 0) {
                deinterlace.mode = DEINTERLACE_MODE_BOB;
            }
            else {
                ERROR("wrong mode deinterlace mode %s", m_deinterlaceMode);
                return false;
            }
            if (m_vpp->setParameters(VppParamTypeDeinterlace, &deinterlace) != YAMI_SUCCESS) {
                ERROR("deinterlace failed for mode %s", m_deinterlaceMode);
                return false;
            }
        }
#endif
        return true;
    }
    SharedPtr<VADisplay> m_display;
    SharedPtr<VppInput> m_input;
    SharedPtr<VppOutput> m_output;
    SharedPtr<FrameAllocator> m_allocator;
    SharedPtr<IVideoPostProcess> m_vpp;
    int32_t m_sharpening;
    int32_t m_denoise;
    char* m_deinterlaceMode;
    char* m_inputName;
    char* m_outputName;
};

void usage()
{
    printf("a tool to do video post process, support scaling and CSC\n");
    printf("we can guess size and color format from your file name\n");
    printf("current supported format are i420, yv12, nv12\n");
    printf("usage: yamivpp <option> input_1920x1080.i420 output_320x240.yv12\n");
    printf("       -s <level> optional, sharpening level\n");
    printf("       --dn <level> optional, denoise level\n");
    printf("       --di <mode>, optional, deinterlace mode, only support bob\n");
}

int main(int argc, char** argv)
{
    VppTest vpp;
    if (!vpp.init(argc, argv)) {
        ERROR("init vpp failed");
        return -1;
    }
    if (!vpp.run()){
        ERROR("run vpp failed");
        return -1;
    }
    printf("vpp done\n");
    return  0;

}
