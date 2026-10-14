#include "pico/stdlib.h"
#include <stdio.h>
#include "ledmatrix.h"
#include "imagedata.h"

#define BRIGHTNESS 2

// Light one pixel and keep the other 63 LED dark - show the wiring of LED
void show_single_led(int position){
    for (int j = 0; j < NUM_LEDS; j++){ //j - send the color to the LED
        if(j == position){
            put_pixel(urgb_u32(BRIGHTNESS, 0, BRIGHTNESS)); // light the chossen LED
        }else{
            put_pixel(0); //pixel stays dark
        }
    }
}

// Start the panel, draw one pixel and show it again and again
int main(){
    stdio_init_all();
    sleep_ms(2000);
    led_init();
    clear_frame();
    /*// Test to display each corners
    set_pixel(0, 0, urgb_u32(BRIGHTNESS, 0, BRIGHTNESS));
    set_pixel(7, 0, urgb_u32(BRIGHTNESS, 0, BRIGHTNESS));
    set_pixel(0, 7, urgb_u32(BRIGHTNESS, 0, BRIGHTNESS));
    set_pixel(7, 7, urgb_u32(BRIGHTNESS, 0, BRIGHTNESS));*/
    
    set_brightness(20);
    show_image(card_heart); //To change image to see the changes in LED

    while(true){
        tight_loop_contents();
    }
}