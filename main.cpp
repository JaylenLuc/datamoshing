#include <iostream>
#include <vector>
#include <cstring>  // for memcpy
#include <fstream>
#include <sstream>
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/avutil.h>
    #include <libswscale/swscale.h>
    #include "moshUtil.h"
}

std::vector<uint8_t> readFile(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
void writeFile(const std::string& filename, const std::vector<uint8_t>& data) {
    std::ofstream out(filename, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
}
int main(int argc, char* argv[]) {
    const short int FPS = 30; 
    const short transition_frame = 100;
    MoshType mosh_type = IFRAMERM;
    const char* output_file = "output.h264";
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <video_file>" << std::endl;
        return 1;
    }
    if (std::string(argv[1]) == "-h") {
        std::cout << "All available options: " << std::endl;
        std::cout << "-i (i frame removal)(defaults to this if no third argument is provided)" << std::endl;
        return 1;
    }

    //*********** Check if the second argument is provided for mosh type and set it accordingly*********/
    if (argc > 2) {
        // if (std::string(argv[2]) == "-p") {
        //     mosh_type = PFRAMEDUP;
        // }
    }
    //****************/
    for (int i = 0; i < argc; ++i) {
        std::cout << "Argument " << i << ": " << argv[i] << std::endl;
    }

    const char* input_file = argv[1];
    // open the input file and allocate the format context
    AVFormatContext* format_context = nullptr;
    if (avformat_open_input(&format_context, input_file, nullptr, nullptr ) < 0) {
        std::cout << "Could not open input file: " << input_file << std::endl;
        return 2;
    }
    std::cout << "Opening video file: " << input_file << std::endl;
    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        std::cout << "Could not find stream information" << std::endl;
        return 3;
    }
    std::cout << "Creating output context for: data_moshed.mp4" << std::endl;
    std::cout << "Finding stream info: " << input_file << std::endl;
    //here we find the video stream
    int HEVC_index = -1;
    for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
            format_context->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264) {
            HEVC_index = i;
            break;
        }
    }

    if (HEVC_index == -1) {
        std::cout << "Could not find HVEC stream" << std::endl;
        return 4;
    }
    AVFormatContext* out_format_ctx = nullptr;
    avformat_alloc_output_context2(&out_format_ctx, nullptr, nullptr, "data_moshed.mp4");
    AVStream* out_stream = avformat_new_stream(out_format_ctx, nullptr);
    AVCodecParameters* in_codecpar = format_context->streams[HEVC_index]->codecpar;
    AVCodecParameters* out_codecpar = out_stream->codecpar;
    AVRational time_base = format_context->streams[HEVC_index]->time_base;
    out_codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_codecpar->codec_id = AV_CODEC_ID_H264;
    out_codecpar->width = in_codecpar->width;
    out_codecpar->height = in_codecpar->height;
    out_codecpar->format = in_codecpar->format;
    out_codecpar->codec_tag = 0;
    out_stream->time_base = time_base;
    int width = in_codecpar->width;
    int height = in_codecpar->height;

    
    std::ofstream out(output_file, std::ios::binary);
    AVPacket pkt;

    while (av_read_frame(format_context, &pkt) >= 0) {
        if (pkt.stream_index == HEVC_index) {
            // Write raw packet data directly (this is still in H264 format inside the MP4)
            out.write(reinterpret_cast<const char*>(pkt.data), pkt.size);
        }
        av_packet_unref(&pkt);
    }

    out.close();

    std::vector<uint8_t> h264_byte_vector = readFile(output_file);
    std::vector<uint8_t> h264_byte_output_vector;
    size_t i = 0;
    while (i + 4 < h264_byte_vector.size()){

        //this is to find the NAL start code
        size_t start = i;
        size_t start_code_len = 0;

        if (h264_byte_vector[i] != 0x00 || h264_byte_vector[i+1] != 0x00) {
            ++i;
            continue;
        }
        if (h264_byte_vector[i+2] == 0x01) {
            start_code_len = 3;
        } else if (h264_byte_vector[i+2] == 0x00 && h264_byte_vector[i+3] == 0x01) {
            start_code_len = 4;
        }
        size_t next = i + start_code_len;
        while (next + 4 < h264_byte_vector.size()) {
            if ((h264_byte_vector[next] == 0x00 && h264_byte_vector[next+1] == 0x00 &&
                 (h264_byte_vector[next+2] == 0x01 || (h264_byte_vector[next+2] == 0x00 && h264_byte_vector[next+3] == 0x01)))) {
                break;
            }
            ++next;
        }
        uint8_t nal_type = h264_byte_vector[i + start_code_len] & 0x1F; //NAL bit masking to isolate unit type 
        //  Type Name
        //     0 [unspecified]
        //     1 Coded slice
        //     2 Data Partition A
        //     3 Data Partition B
        //     4 Data Partition C
        //     5 IDR (Instantaneous Decoding Refresh) Picture
        //     6 SEI (Supplemental Enhancement Information)
        //     7 SPS (Sequence Parameter Set)
        //     8 PPS (Picture Parameter Set)
        //     9 Access Unit Delimiter
        //    10 EoS (End of Sequence)
        //    11 EoS (End of Stream)
        //    12 Filter Data
        // 13-23 [extended]
        // 24-31 [unspecified] 

        if (nal_type != 5) {
            h264_byte_output_vector.insert(
                h264_byte_output_vector.end(), 
                h264_byte_vector.begin() + i, 
                h264_byte_vector.begin() + next
            );
        } else {
            std::cout << "Skipping IDR frame at byte: " << i << std::endl;
        }

        i = next;

    }
    // avformat_close_input(&foramt_context);
    std::cout << "Saved output to: build/" << output_file << std::endl;
    writeFile(output_file, h264_byte_output_vector);

    //remux using temrinal 
    std::stringstream cmd;
    cmd << "ffmpeg -y -s " << width << "x" << height
        << " -framerate 30"
        << " -i output.h264"
        << " -c copy data_moshed.mp4";
    std::system(cmd.str().c_str());
    // avio_open(&out_format_ctx->pb, "data_moshed.mp4", AVIO_FLAG_WRITE);
    // avformat_write_header(out_format_ctx, nullptr);

    return 0;
}