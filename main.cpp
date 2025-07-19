#include <iostream>
#include <vector>
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/avutil.h>
    #include <libswscale/swscale.h>
}
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <video_file>" << std::endl;
        return 1;
    }
    const char* input_file = argv[1];
    // open the input file and allocate the format context
    AVFormatContext* formatContext = nullptr;
    if (avformat_open_input(&formatContext, input_file, nullptr, nullptr ) < 0) {
        std::cout << "Could not open input file: " << input_file << std::endl;
        return 2;
    }
    std::cout << "Opening video file: " << input_file << std::endl;
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        std::cout << "Could not find stream information" << std::endl;
        return 3;
    }
    std::cout << "Finding stream info: " << input_file << std::endl;
    //here we find the video stream
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        std::cout << "Could not find video stream" << std::endl;
        return 4;
    }
    std::cout << "Finding AVMEDIA TYPE VIDEO: " << input_file << std::endl;
    AVCodecParameters* codecpar = formatContext->streams[video_stream_idx]->codecpar;
    // AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_context, codecpar);
    std::cout << "Filling codec context from codec parameters"  << std::endl;
    if (avcodec_open2(codec_context, codec, nullptr) < 0) {
        std::cout << "Could not open codec" << std::endl;
        return 5;
    }
    std::cout << "opening codec"  << std::endl;
    AVFrame* frame = av_frame_alloc();
    //frame gets encoeded into a packet 
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(formatContext, packet) == 0) {
        std::cout << " reading frame" << std::endl;
        if (packet->stream_index == video_stream_idx) {
            //decoding
            int ret = avcodec_send_packet(codec_context, packet);
            if (ret < 0) {
                std::cout << "Error sending packet for decoding: "  << std::endl;
                break;
            }
            while (ret >= 0 ) {
                ret = avcodec_receive_frame(codec_context, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break; // No more frames to decode
                } else if (ret < 0) {
                    std::cout << "Error receiving frame: " << std::endl;
                    break;
                }
                //if still frames left and no error then we can process the frame
                std::cout << "Frame " << codec_context->frame_num
                          << " (type=" << av_get_picture_type_char(frame->pict_type)
                          << " bytes) pts " << frame->pts
                          << "format" << frame->pict_type
                          << std::endl;
                
            }
        }

        av_packet_unref(packet);
    }
    std::cout << "Finished reading frames" << std::endl;
    av_frame_free(&frame);
    avcodec_free_context(&codec_context);
    avformat_close_input(&formatContext);
    return 0;
}