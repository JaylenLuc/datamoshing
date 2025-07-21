#include <iostream>
#include <vector>
#include <cstring>  // for memcpy
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/avutil.h>
    #include <libswscale/swscale.h>
    #include "moshUtil.h"
}

void encode_and_write(AVCodecContext* enc_ctx, AVFormatContext* fmt_ctx, AVStream* stream, AVFrame* frame) {
    if (avcodec_send_frame(enc_ctx, frame) < 0) {
        std::cerr << "Error sending frame to encoder\n";
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;

        if (av_interleaved_write_frame(fmt_ctx, pkt) < 0) {
            std::cerr << "Error writing packet\n";
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

int main(int argc, char* argv[]) {
    const short int FPS = 30; 
    const short transition_frame = 100;
    MoshType mosh_type = IFRAMERM;

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <video_file>" << std::endl;
        return 1;
    }
    if (std::string(argv[1]) == "-h") {
        std::cout << "All available options: " << std::endl;
        std::cout << "-i (i frame removal)(defaults to this if no third argument is provided)" << std::endl;
        std::cout << "-p (p frame duplication)" << std::endl;
        return 1;
    }

    //*********** Check if the second argument is provided for mosh type and set it accordingly*********/
    if (argc > 2) {
        if (std::string(argv[2]) == "-p") {
            mosh_type = PFRAMEDUP;
        }
    }
    //****************/
    for (int i = 0; i < argc; ++i) {
        std::cout << "Argument " << i << ": " << argv[i] << std::endl;
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
    AVFormatContext* outFormatCtx = nullptr;
    avformat_alloc_output_context2(&outFormatCtx, nullptr, nullptr, "data_moshed.mp4");
    std::cout << "Creating output context for: data_moshed.mp4" << std::endl;
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
    std::cout << "Found AVMEDIA TYPE VIDEO: " << input_file << std::endl;

    AVCodecParameters* codecpar = formatContext->streams[video_stream_idx]->codecpar;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    const AVCodec* out_codec = avcodec_find_encoder(codecpar->codec_id);
    
    AVStream* out_stream = avformat_new_stream(outFormatCtx, out_codec);
    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    AVCodecContext* out_codec_ctx = avcodec_alloc_context3(out_codec);

    avcodec_parameters_to_context(codec_context, codecpar);

    // Match encoder params to input
    out_codec_ctx->height = codec_context->height;
    out_codec_ctx->width = codec_context->width;
    out_codec_ctx->sample_aspect_ratio = codec_context->sample_aspect_ratio;
    if (out_codec->pix_fmts) {
        out_codec_ctx->pix_fmt = out_codec->pix_fmts[0];
    } else {
        out_codec_ctx->pix_fmt = codec_context->pix_fmt;
    }
    out_codec_ctx->time_base = (AVRational) {1, FPS};
    out_stream->time_base = out_codec_ctx->time_base;
    out_codec_ctx->max_b_frames = 0;
    out_codec_ctx->has_b_frames = 0;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "sc_threshold", "0", 0);    // Prevent auto keyframe detection
    av_dict_set(&opts, "g", "9999", 0);            // Very high GOP size (delays I-frames)
    av_dict_set(&opts, "bf", "0", 0);              // No B-frames
    av_dict_set(&opts, "crf", "30", 0);   // Lower quality, more visible moshing
    av_dict_set(&opts, "preset", "ultrafast", 0);

    //its time to open the codec encoder context
    if (avcodec_open2(out_codec_ctx, out_codec, &opts) < 0) {
        std::cerr << "Could not open output codec" << std::endl;
        return 6;
    }
    avcodec_parameters_from_context(out_stream->codecpar, out_codec_ctx);
    av_dict_free(&opts);
    // Write header
    if (!(outFormatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_open(&outFormatCtx->pb, "data_moshed.mp4", AVIO_FLAG_WRITE);
    }
    int alloc_stream_res = avformat_write_header(outFormatCtx, nullptr);
    if (alloc_stream_res < 0) {
        std::cerr << "Could not write header to output file" << std::endl;
        return 1;
    }
    std::cout << "Filling codec context from codec parameters"  << std::endl;
    //open decoder context
    if (avcodec_open2(codec_context, codec, nullptr) < 0) {
        std::cout << "Could not open codec" << std::endl;
        return 5;
    }
    std::cout << "opening codec"  << std::endl;
    AVFrame* frame = av_frame_alloc();
    //frame gets encoded into a packet 
    AVPacket* packet = av_packet_alloc();

    AVFrame* last_p_frame = nullptr;
    std::vector<AVFrame*> p_history;
    int64_t pts_counter = 0;
    bool skip_next_i_frame = false;

    unsigned int frame_count = 0;
    std::cout << "*******************arbitration of frames*************************/" << std::endl;
    while (av_read_frame(formatContext, packet) == 0) {
        frame_count++;
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
                    break; 
                } else if (ret < 0) {
                    std::cout << "Error receiving frame: " << std::endl;
                    break;
                }
                //if still frames left and no error then we can process the frame
                char frame_data_type = av_get_picture_type_char(frame->pict_type);

                switch (mosh_type) {
                    case IFRAMERM:
                        if (frame_data_type == 'I') {
                            // std::cout << "Skipping I-frame, reusing last P-frame\n";
                            // if (last_p_frame != nullptr) {
                            //     for (int i = 0; i < 10; i++) {
                            //         AVFrame* reuse = av_frame_clone(last_p_frame);
                            //         reuse->pts = pts_counter++;
                            //         reuse->pict_type = AV_PICTURE_TYPE_P;
                            //         reuse->flags &= ~AV_FRAME_FLAG_KEY;
                            //         encode_and_write(out_codec_ctx, outFormatCtx, out_stream, reuse);
                            //         av_frame_free(&reuse);
                            //     }
                            // }
                            av_frame_unref(frame);
                            continue;
                        } else if (frame_data_type == 'P') {
                            // Store this P-frame for later reference
                            if (last_p_frame != nullptr) {
                                av_frame_free(&last_p_frame);
                            }
                            last_p_frame = av_frame_clone(frame);

                            // Force it to not be a keyframe
                            frame->pict_type = AV_PICTURE_TYPE_P;
                            frame->flags &= ~AV_FRAME_FLAG_KEY;
                            frame->pts = pts_counter++;
                            encode_and_write(out_codec_ctx, outFormatCtx, out_stream, frame);
                        } else if (frame_data_type == 'B') {
                            std::cout << "Skipping B-frame" << std::endl;
                            av_frame_unref(frame);
                            continue;
                        } else {
                            // Just in case: treat unknown as P-frame
                            frame->pict_type = AV_PICTURE_TYPE_P;
                            frame->flags &= ~AV_FRAME_FLAG_KEY;
                            frame->pts = pts_counter++;
                            encode_and_write(out_codec_ctx, outFormatCtx, out_stream, frame);
                        }
                        break;
                        
                    case PFRAMEDUP: {
                        // Skip B-frames entirely
                        if (frame_data_type == 'B') {
                            std::cout << "B-frame detected, skipping" << std::endl;
                            av_frame_unref(frame);
                            continue;
                        }

                        // Check if we're at a transition point
                        const bool is_at_transition = (frame_count != 0 && frame_count % transition_frame == 0);
                        if (is_at_transition) {
                            std::cout << "TRANSITION TRIGGERED at frame " << frame_count << "\n";
                            skip_next_i_frame = true;
                        }

                        // Handle I-frames
                        if (frame_data_type == 'I') {
                            if (skip_next_i_frame) {
                                std::cout << "Creating transition datamosh effect - skipping I-frame\n";
                                
                                // Create multiple corrupted frames for dramatic effect
                                if (last_p_frame != nullptr) {
                                    // Generate several frames with increasing corruption
                                    for (int repeat = 0; repeat < 15; repeat++) {
                                        AVFrame* corrupted = av_frame_clone(last_p_frame);
                                        corrupted->pts = pts_counter++;
                                        corrupted->pict_type = AV_PICTURE_TYPE_P;
                                        corrupted->flags &= ~AV_FRAME_FLAG_KEY;
                                        
                                        // Add some basic corruption by modifying pixel data
                                        if (corrupted->data[0] != nullptr && corrupted->linesize[0] > 0) {
                                            int height = corrupted->height;
                                            int width = corrupted->linesize[0];
                                            
                                            // Create horizontal streaking effect
                                            for (int y = repeat * 10; y < height && y < (repeat + 1) * 10; y++) {
                                                if (y > 0 && y < height - 1) {
                                                    uint8_t* line = corrupted->data[0] + y * width;
                                                    uint8_t* prev_line = corrupted->data[0] + (y-1) * width;
                                                    // Copy previous line to create streaking
                                                    memcpy(line, prev_line, width);
                                                }
                                            }
                                            
                                            // Add some vertical displacement
                                            if (repeat > 5) {
                                                int shift_amount = (repeat - 5) * 2;
                                                for (int y = shift_amount; y < height - shift_amount; y++) {
                                                    uint8_t* dest_line = corrupted->data[0] + y * width;
                                                    uint8_t* src_line = corrupted->data[0] + (y - shift_amount) * width;
                                                    memcpy(dest_line, src_line, width);
                                                }
                                            }
                                        }
                                        
                                        encode_and_write(out_codec_ctx, outFormatCtx, out_stream, corrupted);
                                        av_frame_free(&corrupted);
                                    }
                                    
                                    std::cout << "Generated " << 15 << " corrupted frames for transition\n";
                                } else {
                                    std::cout << "No P-frame available for transition effect\n";
                                }
                                
                                skip_next_i_frame = false;
                                av_frame_unref(frame);
                                continue;
                            } else {
                                // Normal I-frame processing - just encode it
                                frame->pts = pts_counter++;
                                encode_and_write(out_codec_ctx, outFormatCtx, out_stream, frame);
                            }
                        }
                        // Handle P-frames
                        else if (frame_data_type == 'P') {
                            // Store this P-frame for potential future use
                            if (last_p_frame) {
                                av_frame_free(&last_p_frame);
                            }
                            last_p_frame = av_frame_clone(frame);
                            
                            // Encode the P-frame normally
                            frame->pts = pts_counter++;
                            encode_and_write(out_codec_ctx, outFormatCtx, out_stream, frame);
                        }
                        // Handle other frame types
                        else {
                            if (frame->pict_type == AV_PICTURE_TYPE_NONE) {
                                std::cout << "Frame " << codec_context->frame_num << " has no pict_type yet (AV_PICTURE_TYPE_NONE)" << std::endl;
                                av_frame_unref(frame);
                                continue;
                            }
                            // For any other frame type, just encode it normally
                            frame->pts = pts_counter++;
                            encode_and_write(out_codec_ctx, outFormatCtx, out_stream, frame);
                        }
                        break;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    std::cout << "*******************END OF arbitration of frames*************************/" << std::endl;
    
    //flushing and cleanup
    for (AVFrame* p_frame : p_history) {
        av_frame_free(&p_frame);
    }
    p_history.clear();
    
    if (last_p_frame != nullptr) {
        av_frame_free(&last_p_frame);
    }
    
    AVPacket* flush_pkt = av_packet_alloc();
    avcodec_send_frame(out_codec_ctx, nullptr);
    while (avcodec_receive_packet(out_codec_ctx, flush_pkt) == 0) {
        av_packet_rescale_ts(flush_pkt, out_codec_ctx->time_base, out_stream->time_base);
        flush_pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(outFormatCtx, flush_pkt);
        av_packet_unref(flush_pkt);
    }
    av_packet_free(&flush_pkt);

    av_write_trailer(outFormatCtx);
    avio_closep(&outFormatCtx->pb);
    avformat_free_context(outFormatCtx);
    
    std::cout << "Finished reading frames" << std::endl;
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_context);
    avcodec_free_context(&out_codec_ctx);
    avformat_close_input(&formatContext);
    return 0;
}