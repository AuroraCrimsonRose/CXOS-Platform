#include "klogo.h"
#include "logo.h"
#include "fb.h"

void klogo_draw(uint32_t start_x, uint32_t start_y)
{
    uint32_t colors[4];

    colors[0] = 0;
    colors[1] = fb_rgb(255, 85, 85);
    colors[2] = fb_rgb(85, 255, 255);
    colors[3] = fb_rgb(255, 255, 255);

    uint32_t x = 0;
    uint32_t y = 0;

    for (uint32_t run = 0; run < CXLOGO_RUNS; run++)
    {
        uint16_t count = cxlogo_data[run].count;
        uint8_t color = cxlogo_data[run].color;

        for (uint16_t i = 0; i < count; i++)
        {
            if (color)
            {
                fb_put_pixel(
                    start_x + x,
                    start_y + y,
                    colors[color]
                );
            }

            x++;

            if (x >= CXLOGO_WIDTH)
            {
                x = 0;
                y++;
            }
        }
    }
}