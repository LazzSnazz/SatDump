#include "suvi_image_assembler.h"
#include "common/image/image.h"
#include "logger.h"
#include <cmath>
#include <filesystem>
#include <thread>
#include "common/image/processing.h"

namespace goes
{
    namespace grb
    {
        GRBSUVIImageAssembler::GRBSUVIImageAssembler(std::string suvi_dir, products::SUVI::GRBProductSUVI config)
            : suvi_directory(suvi_dir),
              suvi_product(config),
              currentTimeStamp(0)
        {
            hasImage = false;
        }

        GRBSUVIImageAssembler::~GRBSUVIImageAssembler()
        {
            if (hasImage)
                save();
        }

        void GRBSUVIImageAssembler::save()
        {
            time_t time_tt = currentTimeStamp;
            std::tm *timeReadable = gmtime(&time_tt);
            std::string utc_filename = std::to_string(timeReadable->tm_year + 1900) +                                                                               // Year yyyy
                                       (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) + // Month MM
                                       (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "T" +    // Day dd
                                       (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) +          // Hour HH
                                       (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) +             // Minutes mm
                                       (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec)) + "Z";

            std::string filename = "SUVI_" + suvi_product.channel + "_" + utc_filename;
            std::string directory = suvi_directory + "/" + suvi_product.channel + "/";
            std::filesystem::create_directories(directory);

// new stuff (yes I know most of this was done with the help with chatgpt but whatever works. it will get cleaned up more when I get more used to this)
    

    //create our stuff for our detached thread
    auto full_image_copy = std::make_shared<image::Image>(full_image);   // deep copy or shared pointer
    auto suvi_product_copy = suvi_product;  // local copy of struct
    auto directory_copy = directory;
    auto filename_copy = filename;
    auto saving_thread_ptr = saving_thread; 



//start the thread
std::thread([=](){
           image::white_balance(*full_image_copy);
            full_image_copy->to_rgb();
            
// find min and max for dynamic range 
            double min_val = 1e12;
            double max_val = -1e12;
            for (size_t y = 0; y < full_image_copy->height(); ++y)
             for (size_t x = 0; x < full_image_copy->width(); ++x) {
               double v = full_image_copy->get_pixel_bilinear(0, (double)x, (double)y);
                if (v < min_val) min_val = v;
                if (v > max_val) max_val = v;
               }

            double scale = 1.0 / (max_val - min_val + 1e-9);
//set ours tints (WIP)
            std::vector<double> tint;
            if (suvi_product_copy.channel == "Fe094"){
             tint = {0.533, 1.0, 1.0};
            }else if (suvi_product_copy.channel == "Fe132"){
             tint = {0.0, 72.4 / 100, 72.4 / 100};  
            }else if (suvi_product_copy.channel == "Fe171"){
             tint = {1.0, 0.792, 0.184};
            }else if (suvi_product_copy.channel == "Fe195"){
             tint = {100.0/ 100, 49.2 / 100, 0.0};
            }else if (suvi_product_copy.channel == "Fe284"){
             tint = {47.5 / 100, 79.7 / 100, 99.6 / 100};
            }else if (suvi_product_copy.channel == "Fe304"){
             tint = {100.0 / 100, 41.7 / 100, 0};
            }

            double gamma_value = 0.5; // Values less than 1.0 make the image brighter

            //apply ours tints
            for (size_t y = 0; y < full_image_copy->height(); ++y)
             for (size_t x = 0; x < full_image_copy->width(); ++x){
              double raw = full_image_copy->get_pixel_bilinear(0, (double)x, (double)y);
              double v = (raw - min_val) * scale;        // normalize to 0–1
              v = std::pow(v, gamma_value);

              //std::vector<double> color = {v*tint[0], v*tint[1], v*tint[2]};
            std::vector<double> color(3);

            if (v < 0.5) {
             // black → tint (dark half)
             double t = v * 2.0;
             color[0] = t * tint[0];
             color[1] = t * tint[1];
             color[2] = t * tint[2];
            } else {
            // tint → white (bright half)
             double t = (v - 0.5) * 2.0;
             color[0] = tint[0] + (1.0 - tint[0]) * t;
             color[1] = tint[1] + (1.0 - tint[1]) * t;
             color[2] = tint[2] + (1.0 - tint[2]) * t;
         }

full_image_copy->draw_pixel(x, y, color);

              

              full_image_copy->draw_pixel(x, y, color);
              }

//product handling just incase we need it again
            //satdump::ImageProducts suvi_product_data;
            //suvi_product_data.instrument_name = "suvi";
            //suvi_product_data.bit_depth = 16;
            //suvi_product_data.has_timestamps = false;
            //suvi_product_data.timestamp_type = satdump::ImageProducts::TIMESTAMP_
            //suvi_product_data.set_timestamps({ static_cast<double>(currentTimeStamp) });
            //suvi_product_data.images.push_back({"SUVI-" + suvi_product.channel, suvi_product.channel, full_image});
            //suvi_product_data.save(directory);
image::median_blur(*full_image_copy);

            saving_thread_ptr->push(*full_image_copy, std::string(directory_copy + filename_copy));
            }).detach();
        }

        void GRBSUVIImageAssembler::reset()
        {
            int image_width = suvi_product.width;
            int image_height = suvi_product.height;

            full_image = image::Image(16, image_width, image_height, 1);
            full_image.fill(0);
            hasImage = false;
        }

        void GRBSUVIImageAssembler::pushBlock(GRBImagePayloadHeader header, image::Image &block)
        {
            if (block.size() == 0)
                return;

            // Check this is the same image
            if (currentTimeStamp != header.utc_time)
            {
                if (hasImage)
                    save();
                reset();

                currentTimeStamp = header.utc_time;
                hasImage = true;
            }

            // Scale image to full bit depth
            // block <<= 2;

            // Fill
            full_image.draw_image(0, block, header.left_x_coord, header.left_y_coord + header.row_offset_image_block);
        }
    }
}