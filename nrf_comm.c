#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h> 
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <string.h>
#include <stdio.h>
#include "Mirf.h"
#include "Mirf_nRF24L01.h"

//DEVICE definition
#define DEV_ADDR 3 //1 is master, so it is not possible
#define LOW_POWER_ENABLE 1

#ifndef DEV_ADDR
    #error "Device(node) address is not defined! Use DEV_ADDR macro."
#else
    #if DEV_ADDR < 2
        #error "This is not master node - address below 2 is not permitted!"
    #endif
#endif

#define TIMER_3_SEC_PERIOD 300
#define TIMER_60_SEC_PERIOD 6000

#include "onewire.h"
#include "ds18x20.h"

#define SWITCHED_PIN 9
#define SENSOR_0_CALIB_ADDR (uint8_t *)1

#if DEV_ADDR==2
    #define NUM_SENSORS 3
#else
    #define NUM_SENSORS 4
#endif

#define LOW_POWER_SENSOR_TYPE_FLAG 128 //is added to value of DS1820 sensor to sign, that this is a low power device
#define SENSOR_0_TYPE 3 //internal temp
#define SENSOR_1_TYPE 0 //on-off output
#define SENSOR_2_TYPE 4 //dallas 18b20 temp sensor
#define SENSOR_3_TYPE 6 //2 lion in series supply

//adc settings: AREF in + mux on GND
#define REF_VCC_INPUT_INTERNAL _BV(REFS0)  | (_BV(MUX3) | _BV(MUX2) | _BV(MUX1))
//_BV(MUX3) | _BV(MUX2) | _BV(MUX1) | _BV(MUX0)
#define ADC_ON PRR &= ~_BV(PRADC)
#define ADC_OFF PRR |= _BV(PRADC)

mirfPacket volatile inPacket;
mirfPacket volatile outPacket;
uint8_t volatile pinState;
//char buff[30];
uint8_t internalTempCalib;
uint16_t volatile longTimer;
uint8_t volatile timerInterruptTriggered;

#ifdef LOW_POWER_ENABLE
 #define LOW_POWER_USE_DEEP_SLEEP_RX_LOOP 0
 #define LOW_POWER_CYCLES 8 //interval = this_number * 8sec
 uint8_t volatile wdt_timer;
 uint8_t volatile low_power_mode = 0;
 #undef SENSOR_2_TYPE
 #define SENSOR_2_TYPE 4 + LOW_POWER_SENSOR_TYPE_FLAG //dallas 18b20 temp sensor + low power sign
#endif

typedef union {
  uint16_t uint;
  struct {
    uint8_t lsb;
    uint8_t msb;
  };
} IntUnion;

uint8_t volatile adcVal;
volatile IntUnion ds1820Temp;
volatile uint8_t actual_Vcc;

void main() __attribute__ ((noreturn));

//======================================================
void USART_Transmit( char *data, uint8_t len )
{
  for (uint8_t i=0; i < len; i++)
  {
    /* Wait for empty transmit buffer */
    while ( !( UCSR0A & (1<<UDRE0)) );
    /* Put data into buffer, sends the data */
    UDR0 = data[i];
  }
}

//======================================================
ISR(TIMER0_COMPA_vect) {
  	timerInterruptTriggered++;
  	longTimer++;
}

ISR(BADISR_vect) { //just for case
  __asm__("nop\n\t");
}

ISR(ADC_vect) {
	  SMCR = 0; //disable adc sleep and enable normal Idle mode
}

// WATCHDOG interrupt (for cyclic waking of low power device)
#ifdef LOW_POWER_ENABLE
ISR(WDT_vect)
{
    wdt_timer++;
}
#endif

void DS1820StartConversion(void)
{
	DS18X20_start_meas( DS18X20_POWER_EXTERN, NULL );
}

void DS1820WaitForEndConversion_loop(void)
{
	while (DS18X20_conversion_in_progress() == DS18X20_CONVERTING) __asm__("nop\n\t");
}

//uses IDLE sleep mode during waiting for the result
//but can be used ONLY WHEN PERIODIC INTERRUPTS ENABLED
//because there needs to be something that will wake processor up from time to time
void DS1820WaitForEndConversion_sleep(void)
{
	while (DS18X20_conversion_in_progress() == DS18X20_CONVERTING)
	{
		SMCR = 0; //idle sleep mode
		sleep_enable();
		sleep_cpu();
	}

	sleep_disable();
}

void DS1820ReadConversionResult(void)
{
	ow_command( DS18X20_READ, NULL );

    //read 16bit value into uint16
    ds1820Temp.lsb = ow_byte_rd();
    ds1820Temp.msb = ow_byte_rd();

    //do not read rest of bytes from sensor, just reset the line
	ow_reset();
}

void ReadDS1820(void)
{
//read temperature from DS1820 and store it to memory
	DS1820StartConversion();
	DS1820WaitForEndConversion_loop();
	DS1820ReadConversionResult();
}

//======================================================
//WARNING: adc MUX and reference must be set before calling this
void startAdcConversion(void)
{
	  SMCR = 2; //enable ADC noise reduct sleep mode

	  ADCSRA |= _BV(ADSC);  // Start the ADC
	  sleep_enable();
	  sleep_cpu();

	  // Reading register "ADCW" takes care of how to read ADCL and ADCH.
	  //adcVal = ADCW;
      //info: adcw is read outside - just right after finishing this function
}

//======================================================
void setup()
{
  //configure uart0  (57600, 8bits, no parity, 1 stop bit)
  UBRR0H = 0;
  UBRR0L = 16;
  UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
  UCSR0B = _BV(RXEN0) | _BV(TXEN0);

  //read internal temp sensor calibration byte from eeprom
  internalTempCalib = eeprom_read_byte(SENSOR_0_CALIB_ADDR);
  if (internalTempCalib == 0xFF) internalTempCalib = 128;

  //set resolution 0.25C for DS18B20 (write just to scratchpad, not to eeprom)
  ow_reset();
  ow_command(DS18X20_WRITE, NULL);
  ow_byte_wr(0xFF); //1st byte - unused (register Tl)
  ow_byte_wr(0xFF); //2nd byte - unused (register Th)
  ow_byte_wr(0x3F); //3rd byte - resolution 10bits
  ow_reset();

  //start Radio
  Mirf.init();
  Mirf.config();
  Mirf.setDevAddr(DEV_ADDR);
  Mirf.powerUpRx();

  //timer0 10ms period, interrupt enable
  //prescaler 1024, count to 156
  OCR0A = 156;
  OCR0B = 170;
  TCCR0A = 2;
  TCCR0B = 5;
  TIMSK0 = 2;

  //set ADC to read temp from internal sensor, 1.1V reference, prescaler 128
  ADMUX = REF_VCC_INPUT_INTERNAL;
  ADCSRA = (_BV(ADEN) | _BV(ADIE) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0) );  // enable the ADC

  //led13 as output
  //pinMode(SWITCHED_PIN, OUTPUT);
  pinState = 1;
  //digitalWrite(SWITCHED_PIN, pinState);

  //disable unused peripherials
  ACSR |= _BV(ACD); //disable comparator
  PRR = ( _BV(PRTWI) | _BV(PRTIM1) | _BV(PRTIM2) | _BV(PRUSART0) ) ;

}

//======================================================
void main(void)
{
 wdt_disable();

 setup();
 //Read DS1820 for the first time
 //but use the LOOP WAIT function because interrupts are not enabled so the sleep mode would never end
 DS1820StartConversion();
 DS1820WaitForEndConversion_loop();
 DS1820ReadConversionResult();

 sei();

 ADMUX = REF_VCC_INPUT_INTERNAL; //Vcc reference + mux on internal 1.1V
 startAdcConversion();
 actual_Vcc = 56265 / ADCW; // Calculate
 ADC_OFF; //disable ADC again

 memset((void*)&outPacket, 0, sizeof(mirfPacket) );
 //memset(buff, 0, sizeof(buff));

 //debug();
 while(1) {

    #ifdef LOW_POWER_ENABLE
    //low power mode driven by watchdog resets - loop trap
    if (low_power_mode == 1)
    {
        if (wdt_timer == LOW_POWER_CYCLES) { //sleep mode elapsed, turn off and go to normal mode
            ADC_ON; //turn on ADC
            SMCR = 0; //power down mode = off
            WDTCSR = (1<<WDCE) | (1<<WDE);
            WDTCSR = 0; //wdt = off
            wdt_timer = 0;
            low_power_mode = 0;

            //refresh temperature measurement
            //but to not just waste time in wait loop, we will use the 185ms conversion time
            // to do some other useful stuff in the meantime
            DS1820StartConversion();

            // we can read battery voltage during conversion time
            // will use special recipe when measure internal 1.1 with Vcc reference which will
            //allow to count the Vcc from that
            ADMUX = REF_VCC_INPUT_INTERNAL;; //Vcc reference + mux on internal 1.1V
            startAdcConversion();
            // orgiginal formula was 1125300L / ADCW; -> Vcc (in mV); 1125300 = 1.1*1023*1000
            // which is not the best because of using 32bit constant
            // and it will be better to get result which will fit into 1B instead of having it in mV
            // when I dig deeper into the formula I discovered that
            // best is to use 20x smaller constant (fit into 16bit) which will produce values:
            // 255 for 5092mv; or 90 for 1800mV which are our max values. Super!
            // calculation will be done on the server side - just divide by 50 and get value in Volts!
            actual_Vcc = 56265 / ADCW; // Calculate
            ADC_OFF; //disable ADC again

            // enable interrupt from timer again for periodic wake/read/DS1820_sleep func
            TIFR0 = 2; //delete possible interrupt flag of timer0
            TIMSK0 = 2; //activate interrupts for timer0

            //wait the rest of time until conversion finish
            DS1820WaitForEndConversion_sleep();
            DS1820ReadConversionResult();

            //enable Mirf receiver - run after DS1820 reading, because there is short timeout for sending ack and data packets back
            //so it wouldt make sense to catch packets during reading of sensor because those packets would be useless after getting to them
            Mirf.powerUpRx();
            timerInterruptTriggered++; //artificially trigger check of rx/tx queues on chip
        }
        else {  //continue with sleep mode
            WDTCSR |= (1<<WDIE); //interrupt enable flag is atomatically cleared by interrupt for watchdog, must be refreshed
            SMCR = 0b00000100; //power down sleep mode
            sleep_enable();
            sleep_cpu();
        }

        continue;
    }
    #endif

	 // handle periodic checking of MIRF
	 // originally it was located right in the ISR handling function
	 // but maybe it will be easier when it will be here
	 // and this also is possible even when processor is sleeping after each cycle of this WHILE loop
	 // because the timer ISR wakes it so it comes here right after that
	 if (timerInterruptTriggered > 0)
	 {
		 timerInterruptTriggered = 0;
		 Mirf.handleRxLoop();
		 Mirf.handleTxLoop();
	 }

   //zpracovat prichozi packet
   if (Mirf.inPacketReady)
   {
     Mirf.readPacket((mirfPacket*)&inPacket);

     //sprintf((char*)buff, "in: TX:%d,T:%d,C:%d\n", inPacket.txAddr, inPacket.type, inPacket.counter);
     //USART_Transmit((char*)buff, strlen((char*)buff) );

     if ( (PACKET_TYPE)inPacket.type == REQUEST )
	 {
		payloadRequestStruct *req = (payloadRequestStruct*)&inPacket.payload;
		outPacket.type = RESPONSE;
		outPacket.rxAddr = inPacket.txAddr;
		payloadResponseStruct *res = (payloadResponseStruct*)&outPacket.payload;
		res->cmd = req->cmd;
  		res->from_sensor = req->for_sensor;
  		res->len = 1;

  	    if (req->for_sensor == 0) //==== internal temp sensor =====
  	    {
  	    	if (req->cmd == READ)
  	    	{
  	    		ADC_ON;
  	    		ADMUX = (_BV(REFS1) | _BV(REFS0) | _BV(MUX3)); // ref = internal 1.1V + mux on temp sensor
  	    		startAdcConversion();
  	    		ADMUX = REF_VCC_INPUT_INTERNAL; //return back settings which draws less power
  	    		adcVal = ADCW - 19 - internalTempCalib;
  	    		ADC_OFF;
  	    		res->payload[0] = adcVal;
  	    		Mirf.sendPacket((mirfPacket*)&outPacket);
  	    	}
  	    	else if (req->cmd == CALIBRATION_WRITE)
  	    	{
  	    		if (internalTempCalib != req->payload[0])
  	    		{
  	    			internalTempCalib = req->payload[0];
  	    			eeprom_write_byte(SENSOR_0_CALIB_ADDR, req->payload[0]);
  	    		}
  	    	}
  	    	else if (req->cmd == CALIBRATION_READ)
  	    	{
  	    		res->payload[0] = internalTempCalib;
  	    		Mirf.sendPacket((mirfPacket*)&outPacket);
  	    	}

  		}
  		else if (req->for_sensor == 1) //==== door switch =====
  		{
  		  if (req->cmd == WRITE)
  		  {
  		     if (req->payload[0] > 0) pinState = 1; else pinState = 0;
  			 //digitalWrite(SWITCHED_PIN, pinState);
  		  }
  		  else if (req->cmd == READ)
  		  {
  			 res->payload[0] = pinState;
  			 Mirf.sendPacket((mirfPacket*)&outPacket);
  		  }
        }
  		else if (req->for_sensor == 2) //==== dallas 1820 temperature ====
  		{
            //use value stored in memory
			#ifdef LOW_POWER_ENABLE
			 res->len = 3;
			 res->payload[2] = actual_Vcc; //send also Vcc measure
			#else
			 res->len = 2;
			#endif
			res->payload[0] = ds1820Temp.lsb;
			res->payload[1] = ds1820Temp.msb;
			Mirf.sendPacket((mirfPacket*)&outPacket);

            #ifdef LOW_POWER_ENABLE
                //if we are in low power mode, after first response to request for this sensor
                //wait until the packet is really sent and then
                //increase long timer, so in next while loop it will jump right into power down mode,
                //even if whole interval (3sec) didnt elapse yet
                //this should save some power
                //but limit this feature only on SUCCESSFUL sending of packet
                Mirf.handleTxLoop();
                while (Mirf.sendResult == PROCESSING) NOP_ASM
                if (Mirf.sendResult == SUCCESS) { //was it succesfull send?
                    longTimer += TIMER_3_SEC_PERIOD;
                }
            #endif
  		}
  		else if (req->for_sensor == 3) //==== voltage of supply battery ====
  		{ //it is 2 cells in series, so there will be divider /2 on the input (real voltage would be 2x)
  			res->len = 2;
  			Mirf.sendPacket((mirfPacket*)&outPacket);
  		}
	 }
     else if ( (PACKET_TYPE)inPacket.type == PRESENTATION_REQUEST )
     {
 		outPacket.type = PRESENTATION_RESPONSE;
 		payloadPresentationStruct *res = (payloadPresentationStruct*)&outPacket.payload;
 		res->num_sensors = NUM_SENSORS;
   		res->sensor_type[0] = SENSOR_0_TYPE;
   		res->sensor_type[1] = SENSOR_1_TYPE;
   		res->sensor_type[2] = SENSOR_2_TYPE;
   		res->sensor_type[3] = SENSOR_3_TYPE;
   		Mirf.sendPacket((mirfPacket*)&outPacket);
     }

	 if (Mirf.sendingStatus == IN_FIFO)
	 {
		Mirf.handleTxLoop();
	 }

   }

#ifdef LOW_POWER_ENABLE 
   else if (longTimer > TIMER_3_SEC_PERIOD) //3sec period awake (only)
   {
   		longTimer = 0;
        //temperature measurement is refreshed during end of low power mode

        // ENTER POWER DOWN MODE - watchdog timed refresh
        TIMSK0 = 0; // turn off 10ms timer (by disabling its interrupt)
        SMCR = 0b00000100; //power down sleep mode
        WDTCSR = (1<<WDCE) | (1<<WDE) | (1<<WDP3) | (1<<WDP0);
        WDTCSR = (1<<WDIE) | (1<<WDP3) | (1<<WDP0); //8sec timeout of watchdog
        wdt_timer = 0;
        low_power_mode = 1;
        Mirf.powerDown();

        sleep_enable();
        sleep_cpu();
    }
#else
   else if (longTimer > TIMER_60_SEC_PERIOD) //60sec period
   {
   		longTimer = 0;

        //refresh temperature measurement
        ReadDS1820();
   }
#endif
   else //if there is no packet to be processed, we can enter idle mode to save some power
   {
	  #ifdef LOW_POWER_ENABLE
	    #ifdef LOW_POWER_USE_DEEP_SLEEP_RX_LOOP
	      #if LOW_POWER_USE_DEEP_SLEEP_RX_LOOP == 1
			// if we are low power device, we are usig the RX_DR interrupt from NRF
			// so we wil go into POWER DOWN sleep mode instead of IDLE
			// and wait for the interrupt (pin change interrupt) from this source to wake us
			// BUT! there is problem - in power down Timer0 is not running
			// so we need to set and use WDT to emergency wake if there is no Rx at all
	      #endif
	    #endif
	   #endif
	  sleep_enable();
	  sleep_cpu();
	  sleep_disable();
   }
 }

}
