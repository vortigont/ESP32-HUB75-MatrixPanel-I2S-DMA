#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

// Credits: Louis Beaudoin <https://github.com/pixelmatix/SmartMatrix/tree/teensylc>
// and Sprite_TM: 			https://www.esp32.com/viewtopic.php?f=17&t=3188 and https://www.esp32.com/viewtopic.php?f=13&t=3256

/*

    This is example code to driver a p3(2121)64*32 -style RGB LED display. These types of displays do not have memory and need to be refreshed
    continuously. The display has 2 RGB inputs, 4 inputs to select the active line, a pixel clock input, a latch enable input and an output-enable
    input. The display can be seen as 2 64x16 displays consisting of the upper half and the lower half of the display. Each half has a separate 
    RGB pixel input, the rest of the inputs are shared.

    Each display half can only show one line of RGB pixels at a time: to do this, the RGB data for the line is input by setting the RGB input pins
    to the desired value for the first pixel, giving the display a clock pulse, setting the RGB input pins to the desired value for the second pixel,
    giving a clock pulse, etc. Do this 64 times to clock in an entire row. The pixels will not be displayed yet: until the latch input is made high, 
    the display will still send out the previously clocked in line. Pulsing the latch input high will replace the displayed data with the data just 
    clocked in.

    The 4 line select inputs select where the currently active line is displayed: when provided with a binary number (0-15), the latched pixel data
    will immediately appear on this line. Note: While clocking in data for a line, the *previous* line is still displayed, and these lines should
    be set to the value to reflect the position the *previous* line is supposed to be on.

    Finally, the screen has an OE input, which is used to disable the LEDs when latching new data and changing the state of the line select inputs:
    doing so hides any artefacts that appear at this time. The OE line is also used to dim the display by only turning it on for a limited time every
    line.

    All in all, an image can be displayed by 'scanning' the display, say, 100 times per second. The slowness of the human eye hides the fact that
    only one line is showed at a time, and the display looks like every pixel is driven at the same time.

    Now, the RGB inputs for these types of displays are digital, meaning each red, green and blue subpixel can only be on or off. This leads to a
    color palette of 8 pixels, not enough to display nice pictures. To get around this, we use binary code modulation.

    Binary code modulation is somewhat like PWM, but easier to implement in our case. First, we define the time we would refresh the display without
    binary code modulation as the 'frame time'. For, say, a four-bit binary code modulation, the frame time is divided into 15 ticks of equal length.

    We also define 4 subframes (0 to 3), defining which LEDs are on and which LEDs are off during that subframe. (Subframes are the same as a 
    normal frame in non-binary-coded-modulation mode, but are showed faster.)  From our (non-monochrome) input image, we take the (8-bit: bit 7 
    to bit 0) RGB pixel values. If the pixel values have bit 7 set, we turn the corresponding LED on in subframe 3. If they have bit 6 set,
    we turn on the corresponding LED in subframe 2, if bit 5 is set subframe 1, if bit 4 is set in subframe 0.

    Now, in order to (on average within a frame) turn a LED on for the time specified in the pixel value in the input data, we need to weigh the
    subframes. We have 15 pixels: if we show subframe 3 for 8 of them, subframe 2 for 4 of them, subframe 1 for 2 of them and subframe 1 for 1 of
    them, this 'automatically' happens. (We also distribute the subframes evenly over the ticks, which reduces flicker.)

    In this code, we use the I2S peripheral in parallel mode to achieve this. Essentially, first we allocate memory for all subframes. This memory
    contains a sequence of all the signals (2xRGB, line select, latch enable, output enable) that need to be sent to the display for that subframe.
    Then we ask the I2S-parallel driver to set up a DMA chain so the subframes are sent out in a sequence that satisfies the requirement that
    subframe x has to be sent out for (2^x) ticks. Finally, we fill the subframes with image data.

    We use a front buffer/back buffer technique here to make sure the display is refreshed in one go and drawing artifacts do not reach the display.
    In practice, for small displays this is not really necessarily.
    
*/


// For development testing only
//#define IGNORE_REFRESH_RATE 1


#define val2PWM(val) { lumConvTab[(uint8_t)val]; }
// macro's to calculate sizes of a single buffer (double biffer takes twice as this)
#define rowBitStructBuffSize        sizeof(ESP32_I2S_DMA_STORAGE_TYPE) * (PIXELS_PER_ROW + CLKS_DURING_LATCH) * PIXEL_COLOR_DEPTH_BITS
#define frameStructBuffSize         rpf * rowBitStructBuffSize

bool MatrixPanel_I2S_DMA::allocateDMAmemory()
{

   /***
    * Step 1: Look at the overall DMA capable memory for the DMA FRAMEBUFFER data only (not the DMA linked list descriptors yet) 
    *         and do some pre-checks.
    */

    int    _num_frame_buffers                   = (m_cfg.double_buff) ? 2:1;
    size_t _frame_buffer_memory_required        = frameStructBuffSize * _num_frame_buffers; 
    size_t _dma_linked_list_memory_required     = 0; 
    size_t _total_dma_capable_memory_reserved   = 0;   
    
	// 1. Calculate the amount of DMA capable memory that's actually available
    #if SERIAL_DEBUG    
        Serial.printf_P(PSTR("Panel Width: %d pixels.\r\n"),  PIXELS_PER_ROW);
        Serial.printf_P(PSTR("Panel Height: %d pixels.\r\n"), m_cfg.mx_height);

        if (m_cfg.double_buff) {
          Serial.println(F("DOUBLE FRAME BUFFERS / DOUBLE BUFFERING IS ENABLED. DOUBLE THE RAM REQUIRED!"));
        }
        
        Serial.println(F("DMA memory blocks available before any malloc's: "));
        heap_caps_print_heap_info(MALLOC_CAP_DMA);
        
        Serial.printf_P(PSTR("We're going to need %d bytes of SRAM just for the frame buffer(s).\r\n"), _frame_buffer_memory_required);  
        Serial.printf_P(PSTR("The total amount of DMA capable SRAM memory is %d bytes.\r\n"), heap_caps_get_free_size(MALLOC_CAP_DMA));
        Serial.printf_P(PSTR("Largest DMA capable SRAM memory block is %d bytes.\r\n"), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));          
		
    #endif

    // Can we potentially fit the framebuffer into the DMA capable memory that's available?
    if ( heap_caps_get_free_size(MALLOC_CAP_DMA) < _frame_buffer_memory_required  ) {
      
      #if SERIAL_DEBUG      
        Serial.printf_P(PSTR("######### Insufficient memory for requested resolution. Reduce MATRIX_COLOR_DEPTH and try again.\r\n\tAdditional %d bytes of memory required.\r\n\r\n"), (_frame_buffer_memory_required-heap_caps_get_free_size(MALLOC_CAP_DMA)) );
      #endif

      return false;
    }
	
	// Alright, theoretically we should be OK, so let us do this, so
	// lets allocate a chunk of memory for each row (a row could span multiple panels if chaining is in place)
  dma_buff.rowBits.reserve(rpf);

  // iterate through number of rows
	for (int malloc_num =0; malloc_num < rpf; ++malloc_num)
	{
    auto ptr = std::make_shared<rowBitStruct>(PIXELS_PER_ROW, PIXEL_COLOR_DEPTH_BITS, m_cfg.double_buff);

    if (ptr->data == nullptr){
      #if SERIAL_DEBUG
		      Serial.printf_P(PSTR("ERROR: Couldn't malloc rowBitStruct %d! Critical fail.\r\n"), malloc_num);
      #endif
    			return false;
          // TODO: should we release all previous rowBitStructs here???
    }

    dma_buff.rowBits.emplace_back(ptr);     // save new rowBitStruct into rows vector
    ++dma_buff.rows;
    #if SERIAL_DEBUG
        Serial.printf_P(PSTR("Malloc'ing %d bytes of memory @ address %ud for frame row %d.\r\n"), ptr->size()*_num_frame_buffers, (unsigned int)ptr->getDataPtr(), malloc_num);
     #endif

	}

    _total_dma_capable_memory_reserved += _frame_buffer_memory_required;    


  /***
   * Step 2: Calculate the amount of memory required for the DMA engine's linked list descriptors.
   *         Credit to SmartMatrix for this stuff.
   */    
   

    // Calculate what colour depth is actually possible based on memory available vs. required dma linked-list descriptors.
    // aka. Calculate the lowest LSBMSB_TRANSITION_BIT value that will fit in memory
    int numDMAdescriptorsPerRow = 0;
    lsbMsbTransitionBit = 0;
	

    while(1) {
        numDMAdescriptorsPerRow = 1;
        for(int i=lsbMsbTransitionBit + 1; i<PIXEL_COLOR_DEPTH_BITS; i++) {
            numDMAdescriptorsPerRow += (1<<(i - lsbMsbTransitionBit - 1));
        }

        size_t ramrequired = numDMAdescriptorsPerRow * rpf * _num_frame_buffers * sizeof(lldesc_t);
        size_t largestblockfree = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        #if SERIAL_DEBUG  
          Serial.printf_P(PSTR("lsbMsbTransitionBit of %d with %d DMA descriptors per frame row, requires %d bytes RAM, %d available, leaving %d free: \r\n"), lsbMsbTransitionBit, numDMAdescriptorsPerRow, ramrequired, largestblockfree, largestblockfree - ramrequired);
        #endif

        if(ramrequired < largestblockfree)
            break;
            
        if(lsbMsbTransitionBit < PIXEL_COLOR_DEPTH_BITS - 1)
            lsbMsbTransitionBit++;
        else
            break;
    }

    Serial.printf_P(PSTR("Raised lsbMsbTransitionBit to %d/%d to fit in remaining RAM\r\n"), lsbMsbTransitionBit, PIXEL_COLOR_DEPTH_BITS - 1);


   #ifndef IGNORE_REFRESH_RATE	
    // calculate the lowest LSBMSB_TRANSITION_BIT value that will fit in memory that will meet or exceed the configured refresh rate
    while(1) {           
        int psPerClock = 1000000000000UL/m_cfg.i2sspeed;
        int nsPerLatch = ((PIXELS_PER_ROW + CLKS_DURING_LATCH) * psPerClock) / 1000;

        // add time to shift out LSBs + LSB-MSB transition bit - this ignores fractions...
        int nsPerRow = PIXEL_COLOR_DEPTH_BITS * nsPerLatch;

        // add time to shift out MSBs
        for(int i=lsbMsbTransitionBit + 1; i<PIXEL_COLOR_DEPTH_BITS; i++)
            nsPerRow += (1<<(i - lsbMsbTransitionBit - 1)) * (PIXEL_COLOR_DEPTH_BITS - i) * nsPerLatch;

        int nsPerFrame = nsPerRow * rpf;
        int actualRefreshRate = 1000000000UL/(nsPerFrame);
        calculated_refresh_rate = actualRefreshRate;

        #if SERIAL_DEBUG  
          Serial.printf_P(PSTR("lsbMsbTransitionBit of %d gives %d Hz refresh: \r\n"), lsbMsbTransitionBit, actualRefreshRate);        
		    #endif

        if (actualRefreshRate > min_refresh_rate) // HACK Hard Coded: 100
          break;
                  
        if(lsbMsbTransitionBit < PIXEL_COLOR_DEPTH_BITS - 1)
            lsbMsbTransitionBit++;
        else
            break;
    }

    Serial.printf_P(PSTR("Raised lsbMsbTransitionBit to %d/%d to meet minimum refresh rate\r\n"), lsbMsbTransitionBit, PIXEL_COLOR_DEPTH_BITS - 1);
	#endif

  /***
   * Step 2a: lsbMsbTransition bit is now finalised - recalculate the DMA descriptor count required, which is used for
   *          memory allocation of the DMA linked list memory structure.
   */          
    numDMAdescriptorsPerRow = 1;
    for(int i=lsbMsbTransitionBit + 1; i<PIXEL_COLOR_DEPTH_BITS; i++) {
        numDMAdescriptorsPerRow += (1<<(i - lsbMsbTransitionBit - 1));
    }
    #if SERIAL_DEBUG
      Serial.printf_P(PSTR("Recalculated number of DMA descriptors per row: %d\n"), numDMAdescriptorsPerRow);
    #endif

    // Refer to 'DMA_LL_PAYLOAD_SPLIT' code in configureDMA() below to understand why this exists.
    // numDMAdescriptorsPerRow is also used to calcaulte descount which is super important in i2s_parallel_config_t SoC DMA setup. 
    if ( rowBitStructBuffSize > DMA_MAX ) {

        #if SERIAL_DEBUG  
          Serial.printf_P(PSTR("rowColorDepthStruct struct is too large, split DMA payload required. Adding %d DMA descriptors\n"), PIXEL_COLOR_DEPTH_BITS-1);
		    #endif

        numDMAdescriptorsPerRow += PIXEL_COLOR_DEPTH_BITS-1; 
        // Not if numDMAdescriptorsPerRow is even just one descriptor too large, DMA linked list will not correctly loop.
    }


  /***
   * Step 3: Allocate memory for DMA linked list, linking up each framebuffer row in sequence for GPIO output.
   */        

    _dma_linked_list_memory_required = numDMAdescriptorsPerRow * rpf * _num_frame_buffers * sizeof(lldesc_t);
  #if SERIAL_DEBUG
		Serial.printf_P(PSTR("Descriptors for lsbMsbTransitionBit of %d/%d with %d frame rows require %d bytes of DMA RAM with %d numDMAdescriptorsPerRow.\r\n"), lsbMsbTransitionBit, PIXEL_COLOR_DEPTH_BITS - 1, rpf, _dma_linked_list_memory_required, numDMAdescriptorsPerRow);    
	#endif   

    _total_dma_capable_memory_reserved += _dma_linked_list_memory_required;

    // Do a final check to see if we have enough space for the additional DMA linked list descriptors that will be required to link it all up!
    if(_dma_linked_list_memory_required > heap_caps_get_largest_free_block(MALLOC_CAP_DMA)) {
       Serial.println(F("ERROR: Not enough SRAM left over for DMA linked-list descriptor memory reservation! Oh so close!\r\n"));
  
        return false;
    } // linked list descriptors memory check

    // malloc the DMA linked list descriptors that i2s_parallel will need
    desccount = numDMAdescriptorsPerRow * rpf;

    //lldesc_t * dmadesc_a = (lldesc_t *)heap_caps_malloc(desccount * sizeof(lldesc_t), MALLOC_CAP_DMA);
    dmadesc_a = (lldesc_t *)heap_caps_malloc(desccount * sizeof(lldesc_t), MALLOC_CAP_DMA);
    assert("Can't allocate descriptor framebuffer a");
    if(!dmadesc_a) {
        Serial.println(F("ERROR: Could not malloc descriptor framebuffer a."));
        return false;
    }
	
    if (m_cfg.double_buff) // reserve space for second framebuffer linked list
    {
        //lldesc_t * dmadesc_b = (lldesc_t *)heap_caps_malloc(desccount * sizeof(lldesc_t), MALLOC_CAP_DMA);
        dmadesc_b = (lldesc_t *)heap_caps_malloc(desccount * sizeof(lldesc_t), MALLOC_CAP_DMA);
        assert("Could not malloc descriptor framebuffer b.");
        if(!dmadesc_b) {
            Serial.println(F("ERROR: Could not malloc descriptor framebuffer b."));
            return false;
        }
    }

    Serial.println(F("*** ESP32-HUB75-MatrixPanel-I2S-DMA: Memory Allocations Complete ***"));
    Serial.printf_P(PSTR("Total memory that was reserved: %d kB.\r\n"), _total_dma_capable_memory_reserved/1024);
    Serial.printf_P(PSTR("... of which was used for the DMA Linked List(s): %d kB.\r\n"), _dma_linked_list_memory_required/1024);
	
    Serial.printf_P(PSTR("Heap Memory Available: %d bytes total. Largest free block: %d bytes.\r\n"), heap_caps_get_free_size(0), heap_caps_get_largest_free_block(0));
    Serial.printf_P(PSTR("General RAM Available: %d bytes total. Largest free block: %d bytes.\r\n"), heap_caps_get_free_size(MALLOC_CAP_DEFAULT), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));


    #if SERIAL_DEBUG    
        Serial.println(F("DMA capable memory map available after malloc's: "));
        heap_caps_print_heap_info(MALLOC_CAP_DMA);
        delay(1000);
    #endif

    // Just os we know
  	initialized = true;

    return true;

} // end allocateDMAmemory()



void MatrixPanel_I2S_DMA::configureDMA(const HUB75_I2S_CFG& _cfg)
{
    #if SERIAL_DEBUG  
      Serial.println(F("configureDMA(): Starting configuration of DMA engine.\r\n"));
    #endif   


    lldesc_t *previous_dmadesc_a     = 0;
    lldesc_t *previous_dmadesc_b     = 0;
    int current_dmadescriptor_offset = 0;

    // HACK: If we need to split the payload in 1/2 so that it doesn't breach DMA_MAX, lets do it by the color_depth.
    int num_dma_payload_color_depths = PIXEL_COLOR_DEPTH_BITS;
    if ( rowBitStructBuffSize > DMA_MAX ) {
        num_dma_payload_color_depths = 1;
    }

    // Fill DMA linked lists for both frames (as in, halves of the HUB75 panel) and if double buffering is enabled, link it up for both buffers.
    for(int row = 0; row < rpf; row++) {

        #if SERIAL_DEBUG          
          Serial.printf_P(PSTR( "Row %d DMA payload of %d bytes. DMA_MAX is %d.\n"), row, dma_buff.rowBits[row]->size(), DMA_MAX);
        #endif

        
        // first set of data is LSB through MSB, single pass (IF TOTAL SIZE < DMA_MAX) - all color bits are displayed once, which takes care of everything below and inlcluding LSBMSB_TRANSITION_BIT
        // NOTE: size must be less than DMA_MAX - worst case for library: 16-bpp with 256 pixels per row would exceed this, need to break into two
        link_dma_desc(&dmadesc_a[current_dmadescriptor_offset], previous_dmadesc_a, dma_buff.rowBits[row]->getDataPtr(), dma_buff.rowBits[row]->size(num_dma_payload_color_depths));
          previous_dmadesc_a = &dmadesc_a[current_dmadescriptor_offset];

        if (m_cfg.double_buff) {
          link_dma_desc(&dmadesc_b[current_dmadescriptor_offset], previous_dmadesc_b, dma_buff.rowBits[row]->getDataPtr(0, 1), dma_buff.rowBits[row]->size(num_dma_payload_color_depths));
          previous_dmadesc_b = &dmadesc_b[current_dmadescriptor_offset]; }

        current_dmadescriptor_offset++;

        // If the number of pixels per row is too great for the size of a DMA payload, so we need to split what we were going to send above.
        if ( rowBitStructBuffSize > DMA_MAX )
        {
          #if SERIAL_DEBUG     
              Serial.printf_P(PSTR("Spliting DMA payload for %d color depths into %d byte payloads.\r\n"), PIXEL_COLOR_DEPTH_BITS-1, rowBitStructBuffSize/PIXEL_COLOR_DEPTH_BITS );
          #endif
          
          for (int cd = 1; cd < PIXEL_COLOR_DEPTH_BITS; cd++) 
          {
            // first set of data is LSB through MSB, single pass - all color bits are displayed once, which takes care of everything below and inlcluding LSBMSB_TRANSITION_BIT
            // TODO: size must be less than DMA_MAX - worst case for library: 16-bpp with 256 pixels per row would exceed this, need to break into two
            link_dma_desc(&dmadesc_a[current_dmadescriptor_offset], previous_dmadesc_a, dma_buff.rowBits[row]->getDataPtr(cd, 0), dma_buff.rowBits[row]->size(num_dma_payload_color_depths) );
            previous_dmadesc_a = &dmadesc_a[current_dmadescriptor_offset];

            if (m_cfg.double_buff) {
              link_dma_desc(&dmadesc_b[current_dmadescriptor_offset], previous_dmadesc_b, dma_buff.rowBits[row]->getDataPtr(cd, 1), dma_buff.rowBits[row]->size(num_dma_payload_color_depths));
              previous_dmadesc_b = &dmadesc_b[current_dmadescriptor_offset]; }

            current_dmadescriptor_offset++;     

          } // additional linked list items           
        }  // row depth struct


        for(int i=lsbMsbTransitionBit + 1; i<PIXEL_COLOR_DEPTH_BITS; i++) 
        {
            // binary time division setup: we need 2 of bit (LSBMSB_TRANSITION_BIT + 1) four of (LSBMSB_TRANSITION_BIT + 2), etc
            // because we sweep through to MSB each time, it divides the number of times we have to sweep in half (saving linked list RAM)
            // we need 2^(i - LSBMSB_TRANSITION_BIT - 1) == 1 << (i - LSBMSB_TRANSITION_BIT - 1) passes from i to MSB

          #if SERIAL_DEBUG  
            Serial.printf_P(PSTR("configureDMA(): DMA Loops for PIXEL_COLOR_DEPTH_BITS %d is: %d.\r\n"), i, (1<<(i - lsbMsbTransitionBit - 1)));
          #endif  

            for(int k=0; k < (1<<(i - lsbMsbTransitionBit - 1)); k++) 
            {
                link_dma_desc(&dmadesc_a[current_dmadescriptor_offset], previous_dmadesc_a, dma_buff.rowBits[row]->getDataPtr(i, 0), dma_buff.rowBits[row]->size(PIXEL_COLOR_DEPTH_BITS - i) );
                previous_dmadesc_a = &dmadesc_a[current_dmadescriptor_offset];

                if (m_cfg.double_buff) {
                  link_dma_desc(&dmadesc_b[current_dmadescriptor_offset], previous_dmadesc_b, dma_buff.rowBits[row]->getDataPtr(i, 1), dma_buff.rowBits[row]->size(PIXEL_COLOR_DEPTH_BITS - i) );
                  previous_dmadesc_b = &dmadesc_b[current_dmadescriptor_offset];
                }
        
                current_dmadescriptor_offset++;

            } // end color depth ^ 2 linked list
        } // end color depth loop

    } // end frame rows

   #if SERIAL_DEBUG  
      Serial.printf_P(PSTR("configureDMA(): Configured LL structure. %d DMA Linked List descriptors populated.\r\n"), current_dmadescriptor_offset);
	  
	  if ( desccount != current_dmadescriptor_offset)
	  {
		Serial.printf_P(PSTR("configureDMA(): ERROR! Expected descriptor count of %d != actual DMA descriptors of %d!\r\n"), desccount, current_dmadescriptor_offset);		  
	  }
    #endif  

    //End markers for DMA LL
    dmadesc_a[desccount-1].eof = 1;
    dmadesc_a[desccount-1].qe.stqe_next=(lldesc_t*)&dmadesc_a[0];

    if (m_cfg.double_buff) {    
      dmadesc_b[desccount-1].eof = 1;
      dmadesc_b[desccount-1].qe.stqe_next=(lldesc_t*)&dmadesc_b[0]; 
    } else {
      dmadesc_b = dmadesc_a; // link to same 'a' buffer
    }

    
    Serial.println(F("Performing I2S setup:"));
	
    i2s_parallel_config_t cfg={
        .gpio_bus={_cfg.gpio.r1, _cfg.gpio.g1, _cfg.gpio.b1, _cfg.gpio.r2, _cfg.gpio.g2, _cfg.gpio.b2, _cfg.gpio.lat, _cfg.gpio.oe, _cfg.gpio.a, _cfg.gpio.b, _cfg.gpio.c, _cfg.gpio.d, _cfg.gpio.e, -1, -1, -1},
        .gpio_clk=_cfg.gpio.clk,
        .clkspeed_hz=_cfg.i2sspeed,   //ESP32_I2S_CLOCK_SPEED,  formula used is 80000000L/(cfg->clkspeed_hz + 1), must result in >=2.  Acceptable values 26.67MHz, 20MHz, 16MHz, 13.34MHz...
        .bits=ESP32_I2S_DMA_MODE,     //ESP32_I2S_DMA_MODE,
        .bufa=0,
        .bufb=0,
        desccount,
        desccount,
        dmadesc_a,
        dmadesc_b
    };

    //Setup I2S
    i2s_parallel_setup_without_malloc(&I2S1, &cfg);

    #if SERIAL_DEBUG  
      Serial.println(F("configureDMA(): DMA configuration completed on I2S1."));
      Serial.println(F("DMA Memory Map after DMA LL allocations:"));
      heap_caps_print_heap_info(MALLOC_CAP_DMA);        

      delay(1000);
    #endif       
		
} // end initMatrixDMABuff


/* There are 'bits' set in the frameStruct that we simply don't need to set every single time we change a pixel / DMA buffer co-ordinate.
 * 	For example, the bits that determine the address lines, we don't need to set these every time. Once they're in place, and assuming we
 *  don't accidently clear them, then we don't need to set them again.
 *  So to save processing, we strip this logic out to the absolute bare minimum, which is toggling only the R,G,B pixels (bits) per co-ord.
 *
 *  Critical dependency: That 'updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)' has been run at least once over the
 *                       entire frameBuffer to ensure all the non R,G,B bitmasks are in place (i.e. like OE, Address Lines etc.)
 *
 *  Note: If you change the brightness with setBrightness() you MUST then clearScreen() and repaint / flush the entire framebuffer.
 */

/* Update a specific co-ordinate in the DMA buffer */
void MatrixPanel_I2S_DMA::updateMatrixDMABuffer(int16_t x_coord, int16_t y_coord, uint8_t red, uint8_t green, uint8_t blue)
{
    if ( !initialized ) { 
      #if SERIAL_DEBUG 
              Serial.println(F("Cannot updateMatrixDMABuffer as setup failed!"));
      #endif         
      return;
    }

  /* 1) Check that the co-ordinates are within range, or it'll break everything big time.
  * Valid co-ordinates are from 0 to (MATRIX_XXXX-1)
  */
  if ( x_coord < 0 || y_coord < 0 || x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height) {
    return;
  }

  /* LED Brightness Compensation. Because if we do a basic "red & mask" for example, 
	 * we'll NEVER send the dimmest possible colour, due to binary skew.
	 * i.e. It's almost impossible for color_depth_idx of 0 to be sent out to the MATRIX unless the 'value' of a color is exactly '1'
   * https://ledshield.wordpress.com/2012/11/13/led-brightness-to-your-eye-gamma-correction-no/
	 */
	red   = lumConvTab[red];
	green = lumConvTab[green];
	blue  = lumConvTab[blue]; 	

	/* When using the drawPixel, we are obviously only changing the value of one x,y position, 
	 * however, the two-scan panels paint TWO lines at the same time
	 * and this reflects the parallel in-DMA-memory data structure of uint16_t's that are getting
	 * pumped out at high speed.
	 * 
	 * So we need to ensure we persist the bits (8 of them) of the uint16_t for the row we aren't changing.
	 * 
	 * The DMA buffer order has also been reversed (refer to the last code in this function)
	 * so we have to check for this and check the correct position of the MATRIX_DATA_STORAGE_TYPE
	 * data.
	 */
    bool painting_top_frame = true;
    if ( y_coord >= rpf) // co-ords start at zero, y_coord = 15 => 16 (rows per frame)
    {
        y_coord -= rpf;  // Subtract the ROWS_PER_FRAME from the pixel co-ord to get the panel ROW (not really the 'y_coord' anymore)
        painting_top_frame = false;
    }
	
    // We need to update the correct uint16_t in the rowBitStruct array, that gets sent out in parallel
    // 16 bit parallel mode - Save the calculated value to the bitplane memory in reverse order to account for I2S Tx FIFO mode1 ordering
    uint16_t rowBitStruct_x_coord_uint16_t_position = (x_coord % 2) ? (x_coord-1):(x_coord+1);

    for(uint8_t color_depth_idx=0; color_depth_idx<PIXEL_COLOR_DEPTH_BITS; color_depth_idx++)  // color depth - 8 iterations
    {
        uint8_t mask = (1 << color_depth_idx); // 24 bit color
        
        // Get the contents at this address,
        // it would represent a vector pointing to the full row of pixels for the specified color depth bit
        ESP32_I2S_DMA_STORAGE_TYPE *p = dma_buff.rowBits[y_coord]->getDataPtr(color_depth_idx, back_buffer_id);

        // We need to update the correct uint16_t word in the rowBitStruct array poiting to a specific pixel at X - coordinate
        uint16_t &v = p[rowBitStruct_x_coord_uint16_t_position];

        if (painting_top_frame)
        { // Need to copy what the RGB status is for the bottom pixels
          v &= BITMASK_RGB1_CLEAR; // reset R1G1B1 bits
          // Set the color of the pixel of interest
          if (green & mask) {  v|=BIT_G1; }
          if (blue & mask)  {  v|=BIT_B1; }
          if (red & mask)   {  v|=BIT_R1; }

        } else { // Do it the other way around 
          v &= BITMASK_RGB2_CLEAR; // reset R2G2B2 bits
          // Color to set
          if (red & mask)   { v|=BIT_R2; }
          if (green & mask) { v|=BIT_G2; }
          if (blue & mask)  { v|=BIT_B2; }
        } // paint

        if (m_cfg.fastmode)
          continue;

        // update address/control bits
        v &= BITMASK_CTRL_CLEAR;      // reset ABCDE,EO,LAT address bits
		
        // normally output current rows ADDX, special case for LSB, output previous row's ADDX (as previous row is being displayed for one latch cycle)
        // uint16_t _y = (color_depth_idx == 0) ? 
        //  gpioRowAddress = y_coord-1;
		
        //uint16_t _y = color_depth_idx ? y_coord : y_coord -1;
		uint16_t _y = y_coord;
        v|=_y << BITS_ADDR_OFFSET;         // shift row coord to match ABCDE bits from bit positions 8 to 12 and set bitvector

        // drive latch while shifting out last bit of RGB data
        if((x_coord) == PIXELS_PER_ROW-1) v|=BIT_LAT;

        // need to disable OE after latch to hide row transition
        // OR one clock before latch, otherwise can get ghosting
        if((x_coord) == 0 || (x_coord)==PIXELS_PER_ROW-2){ v|=BIT_OE; continue;}

        if((color_depth_idx > lsbMsbTransitionBit || !color_depth_idx) && ((x_coord) >= brightness))
          {v|=BIT_OE; continue;}// For Brightness

        // special case for the bits *after* LSB through (lsbMsbTransitionBit) - OE is output after data is shifted, so need to set OE to fractional brightness
        if(color_depth_idx && color_depth_idx <= lsbMsbTransitionBit) {
          // divide brightness in half for each bit below lsbMsbTransitionBit
          int lsbBrightness = brightness >> (lsbMsbTransitionBit - color_depth_idx + 1);
          if((x_coord) >= lsbBrightness) v|=BIT_OE; // For Brightness
        }
		/*
		// Development / testing code only.
		Serial.printf("r value of %d, color depth: %d, mask: %d\r\n", red,	color_depth_idx, mask);
		if (red & mask) { Serial.println("Success - Binary"); v|=BIT_R1; }
		Serial.printf("val2pwm r value:  %d\r\n", val2PWM(red));
		if (val2PWM(red) & mask) { Serial.println("Success - PWM"); v|=BIT_R2; }
		*/
    } // color depth loop (8)
		

} // updateMatrixDMABuffer (specific co-ords change)


/* Update the entire buffer with a single specific colour - quicker */
void MatrixPanel_I2S_DMA::updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)
{
  if ( !initialized ) return;
  
	/* https://ledshield.wordpress.com/2012/11/13/led-brightness-to-your-eye-gamma-correction-no/ */	 
	/*
	red 	= val2PWM(red);
	green 	= val2PWM(green);
	blue 	= val2PWM(blue);  
	*/
	red 	= lumConvTab[red];
	green	= lumConvTab[green];
	blue 	= lumConvTab[blue]; 	

  for(uint8_t color_depth_idx=0; color_depth_idx<PIXEL_COLOR_DEPTH_BITS; color_depth_idx++)  // color depth - 8 iterations
  {
    // let's precalculate RGB1 and RGB2 bits than flood it over the entire DMA buffer
    uint16_t RGB_output_bits = 0;
    uint8_t mask = (1 << color_depth_idx); // 24 bit color

	/* Per the .h file, the order of the output RGB bits is:
	 * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1	  */
    RGB_output_bits |= (bool)(blue & mask);   // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green & mask);  // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red & mask);    // BGR
	
	// Duplicate and shift across so we have have 6 populated bits of RGB1 and RGB2 pin values suitable for DMA buffer
    RGB_output_bits |= RGB_output_bits << BITS_RGB2_OFFSET;  //BGRBGR
	
    //Serial.printf("Fill with: 0x%#06x\n", RGB_output_bits);

    // iterate rows
    for (uint16_t matrix_frame_parallel_row = 0; matrix_frame_parallel_row < rpf; matrix_frame_parallel_row++) // half height - 16 iterations
    {
      // The destination for the pixel row bitstream
      ESP32_I2S_DMA_STORAGE_TYPE *p = dma_buff.rowBits[matrix_frame_parallel_row]->getDataPtr(color_depth_idx, back_buffer_id);

      // iterate pixels in a row
      if (m_cfg.fastmode){
		  
        for(uint16_t x_coord=0; x_coord < PIXELS_PER_ROW; x_coord++) {
          uint16_t &v = p[(x_coord % 2) ? (x_coord-1):(x_coord+1)]; // take reference to a specific pixel (two actually)
          v &= BITMASK_RGB12_CLEAR;  // reset color bits
          v |= RGB_output_bits;     // set new color bits
        }
		
      } else {

        // Set ABCDE address bits vector
        //uint16_t _y = color_depth_idx ? matrix_frame_parallel_row : matrix_frame_parallel_row -1;
		uint16_t _y = matrix_frame_parallel_row;
        _y <<= BITS_ADDR_OFFSET;    // shift row y-coord to match ABCDE bits in vector from 8 to 12

        for(uint16_t x_coord=0; x_coord < PIXELS_PER_ROW; x_coord++) {
			
  	      // We need to update the correct uint16_t in the rowBitStruct array, that gets sent out in parallel
	        // 16 bit parallel mode - Save the calculated value to the bitplane memory in reverse order to account for I2S Tx FIFO mode1 ordering
	        uint16_t rowBitStruct_x_coord_uint16_t_position = (x_coord % 2) ? (x_coord-1):(x_coord+1);
          uint16_t &v = p[rowBitStruct_x_coord_uint16_t_position]; // persist what we already have
          v = RGB_output_bits;    // set colot bits and reset all others
          v|=_y;                  // set ABCDE address bits for current row

          // drive latch while shifting out last bit of RGB data
          if((x_coord) == PIXELS_PER_ROW-1) v|=BIT_LAT;

          // need to disable OE after latch to hide row transition
          // OR one clock before latch, otherwise can get ghosting
          if(!x_coord || (x_coord)==PIXELS_PER_ROW-2){
            v|=BIT_OE; continue;
          }

          // BRT OE
          if((color_depth_idx > lsbMsbTransitionBit || !color_depth_idx) && ((x_coord) >= brightness)){
            v|=BIT_OE; continue;  // For Brightness control
          }

          // special case for the bits *after* LSB through (lsbMsbTransitionBit) - OE is output after data is shifted, so need to set OE to fractional brightness
          if(color_depth_idx && color_depth_idx <= lsbMsbTransitionBit) {
            // divide brightness in half for each bit below lsbMsbTransitionBit
            int lsbBrightness = brightness >> (lsbMsbTransitionBit - color_depth_idx + 1);
            if((x_coord) >= lsbBrightness) v|=BIT_OE; // For Brightness
          }
        } // end of x-iterator
      } // end x_coord iteration
    } // end row iteration
  } // colour depth loop (8)
} // updateMatrixDMABuffer (full frame paint)


#define CLK_PULSE          digitalWrite(_cfg.gpio.clk, HIGH); digitalWrite(_cfg.gpio.clk, LOW);

/**
 * pre-init procedures for specific drivers
 * 
 */
void MatrixPanel_I2S_DMA::shiftDriver(const HUB75_I2S_CFG& _cfg){
    switch (_cfg.driver){
    case HUB75_I2S_CFG::ICN2038S:
    case HUB75_I2S_CFG::FM6124:
    case HUB75_I2S_CFG::FM6126A:
    {
      #if SERIAL_DEBUG 
        Serial.println( F("MatrixPanel_I2S_DMA - initializing FM6124 driver..."));
      #endif
      bool REG1[16] = {0,0,0,0,0, 1,1,1,1,1,1, 0,0,0,0,0};    // this sets global matrix brightness power
      bool REG2[16] = {0,0,0,0,0, 0,0,0,0,1,0, 0,0,0,0,0};    // a single bit enables the matrix output

      for (uint8_t _pin:{_cfg.gpio.r1, _cfg.gpio.r2, _cfg.gpio.g1, _cfg.gpio.g2, _cfg.gpio.b1, _cfg.gpio.b2, _cfg.gpio.clk, _cfg.gpio.lat, _cfg.gpio.oe}){
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
      }

      digitalWrite(_cfg.gpio.oe, HIGH); // Disable Display

      // Send Data to control register REG1
      // this sets the matrix brightness actually
      for (int l = 0; l < PIXELS_PER_ROW; l++){
        for (uint8_t _pin:{_cfg.gpio.r1, _cfg.gpio.r2, _cfg.gpio.g1, _cfg.gpio.g2, _cfg.gpio.b1, _cfg.gpio.b2})
          digitalWrite(_pin, REG1[l%16]);   // we have 16 bits shifters and write the same value all over the matrix array

          if (l > PIXELS_PER_ROW - 12){         // pull the latch 11 clocks before the end of matrix so that REG1 starts counting to save the value
              digitalWrite(_cfg.gpio.lat, HIGH);
          }
          CLK_PULSE
      }

      // drop the latch and save data to the REG1 all over the FM6124 chips
      digitalWrite(_cfg.gpio.lat, LOW);

      // Send Data to control register REG2 (enable LED output)
      for (int l = 0; l < PIXELS_PER_ROW; l++){
        for (uint8_t _pin:{_cfg.gpio.r1, _cfg.gpio.r2, _cfg.gpio.g1, _cfg.gpio.g2, _cfg.gpio.b1, _cfg.gpio.b2})
          digitalWrite(_pin, REG2[l%16]);   // we have 16 bits shifters and we write the same value all over the matrix array

          if (l > PIXELS_PER_ROW - 13){       // pull the latch 12 clocks before the end of matrix so that reg2 stars counting to save the value
              digitalWrite(_cfg.gpio.lat, HIGH);
          }
          CLK_PULSE
      }

      // drop the latch and save data to the REG1 all over the FM6126 chips
      digitalWrite(_cfg.gpio.lat, LOW);

      // blank data regs to keep matrix clear after manipulations
      for (uint8_t _pin:{_cfg.gpio.r1, _cfg.gpio.r2, _cfg.gpio.g1, _cfg.gpio.g2, _cfg.gpio.b1, _cfg.gpio.b2})
        digitalWrite(_pin, LOW);

      for (int l = 0; l < PIXELS_PER_ROW; ++l){
        CLK_PULSE
      }

      digitalWrite(_cfg.gpio.lat, HIGH);
      CLK_PULSE
      digitalWrite(_cfg.gpio.lat, LOW);
      digitalWrite(_cfg.gpio.oe, LOW); // Enable Display
      CLK_PULSE
    }
      break;
    case HUB75_I2S_CFG::SHIFT:
    default:
      break;
    }
}

/**
 * clear screen to black and reset service bits
 */
void MatrixPanel_I2S_DMA::clearScreen(){
  if (m_cfg.fastmode) {
    m_cfg.fastmode = false;       // we always clear screen in 'slow' mode to update all bits in DMA buffer
    updateMatrixDMABuffer(0, 0, 0);
    m_cfg.fastmode = true;        // restore fastmode
  } else {
    updateMatrixDMABuffer(0, 0, 0);
  }
}