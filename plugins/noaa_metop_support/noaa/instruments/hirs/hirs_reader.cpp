#include "hirs_reader.h"
#include <cmath>
#include "common/repack.h"
//#include "iostream"

namespace noaa
{
    namespace hirs
    {
        HIRSReader::HIRSReader()
        {
            for (int i = 0; i < 20; i++)
                channels[i] = new unsigned short[1000 * 56];
        }

        HIRSReader::~HIRSReader()
        {
            for (int i = 0; i < 20; i++)
                delete[] channels[i];
            // delete[] imageBuffer;
        }

        void HIRSReader::work(uint8_t *buffer)
        {
            // get TIP timestamp
            uint16_t mf = ((buffer[4] & 1) << 8) | (buffer[5]);

            if (mf == 0)
            {
                int days = (buffer[8] << 1) | (buffer[9] >> 7);
                uint64_t millisec = ((buffer[9] & 0b00000111) << 24) | (buffer[10] << 16) | (buffer[11] << 8) | buffer[12];
                last_timestamp = ttp.getTimestamp(days, millisec);
            }

            uint8_t HIRS_data[36] = {};
            int pos = 0;
            for (int j : HIRSPositions)
            {
                HIRS_data[pos] = buffer[j];
                pos++;
                // std::cout<<pos<<std::endl;
            }

            uint16_t enct = ((HIRS_data[2] % (int)pow(2, 5)) << 1) | (HIRS_data[3] >> 7);
            // std::cout << "element number:" << enct << " encoder position:" << (unsigned int)HIRS_data[i][0] << std::endl;

            if (enct < 56 && (HIRS_data[35] & 0b10) >> 1 && (HIRS_data[3] & 0x40) >> 6)
            { 
                int current = ((buffer[22] % (int)pow(2, 5)) << 1) | (buffer[23] >> 7);
                // std::cout<<last << ", " << enct << ", " << current <<std::endl;

                uint16_t words13bit[20] = {0};
                uint8_t tmp[32];
                shift_array_left(&HIRS_data[3], 32, 2, tmp);
                repackBytesTo13bits(tmp, 32, words13bit);

                for (int i = 0; i < 20; i++)
                    imageBuffer[HIRSChannels[i]][enct][line] = words13bit[i];

                for (int i = 0; i < 20; i++)
                {
                    if ((imageBuffer[i][enct][line] >> 12) == 1)
                    {
                        imageBuffer[i][enct][line] = (imageBuffer[i][enct][line] & 0b0000111111111111) + 4095;
                    }
                    else
                    {
                        int buffer = 4096 - ((imageBuffer[i][enct][line] & 0b0000111111111111));
                        imageBuffer[i][enct][line] = abs(buffer);
                    }

                    channels[i][55 - enct + 56 * line] = HIRS_data[0] <= 56 ? imageBuffer[i][enct][line] : 0;
                }

                                if (current == 55)
                {
                    line++;
                    if (!contains(timestamps, last_timestamp + (double)(mf/64) * (last_timestamp != -1 ? 6.4 : 0)))
                        timestamps.push_back(last_timestamp + (double)(mf/64) * (last_timestamp != -1 ? 6.4 : 0));
                    else timestamps.push_back(-1);
                }
                // last = enct;
            }
        }

        image::Image<uint16_t> HIRSReader::getChannel(int channel)
        {
            return image::Image<uint16_t>(channels[channel], 56, line, 1);
        }
    } // namespace hirs
} // namespace noaa