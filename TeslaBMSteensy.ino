#include <chip.h>

#define REG_DEV_STATUS      0
#define REG_GPAI            1
#define REG_VCELL1          3
#define REG_VCELL2          5
#define REG_VCELL3          7
#define REG_VCELL4          9
#define REG_VCELL5          0xB
#define REG_VCELL6          0xD
#define REG_TEMPERATURE1    0xF
#define REG_TEMPERATURE2    0x11
#define REG_ALERT_STATUS    0x20
#define REG_FAULT_STATUS    0x21
#define REG_COV_FAULT       0x22
#define REG_CUV_FAULT       0x23
#define REG_ADC_CTRL        0x30
#define REG_IO_CTRL         0x31
#define REG_BAL_CTRL        0x32
#define REG_BAL_TIME        0x33
#define REG_ADC_CONV        0x34
#define REG_ADDR_CTRL       0x3B

float cellVolt[63][6];          // calculated as 16 bit value * 6.250 / 16383 = volts
float moduleVolt[63];           // calculated as 16 bit value * 33.333 / 16383 = volts
float packVolt;                 // All modules added together
uint16_t temperatures[63][2];   // Storage for temperature readings
uint8_t serBuff[128];
uint8_t boards[63];

enum BOARD_STATUS 
{
    BS_STARTUP,       //Haven't tried to find this ID yet
    BS_FOUND,         //Something responded to the ID
    BS_MISSING        //Nobody responded
};


uint8_t genCRC(uint8_t *input, int lenInput)
{
    uint8_t generator = 0x07;
    uint8_t crc = 0;

    for (int x = 0; x < lenInput; x++)
    {
        crc ^= input[x]; /* XOR-in the next input byte */

        for (int i = 0; i < 8; i++)
        {
            if ((crc & 0x80) != 0)
            {
                crc = (uint8_t)((crc << 1) ^ generator);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

void sendData(uint8_t *data, uint8_t dataLen, bool isWrite)
{
    if (isWrite)
    {
      data[0] = data[0] | 1;
    }
    Serial3.write(data, dataLen);
    if (isWrite) Serial3.write(genCRC(data, dataLen));

    Serial.print("Sending: ");
    for (int x = 0; x < dataLen; x++) {
        Serial.print(data[x], HEX);
        Serial.print(" ");
    }
    Serial.println(genCRC(data, dataLen), HEX);
}

int getReply(uint8_t *data)
{  
    int numBytes = 0; 
    Serial.print("Reply: ");
    while (Serial3.available())
    {
      data[numBytes]=Serial3.read();
        Serial.print(data[numBytes], HEX);
        numBytes++;
    }
    Serial.println();
    return numBytes;
}

/*
 * Try to set up any unitialized boards. Send a command to address 0 and see if there is a response. If there is then there is
 * still at least one unitialized board. Go ahead and give it the first ID not registered as already taken.
 * If we send a command to address 0 and no one responds then every board is inialized and this routine stops.
 * Don't run this routine until after the boards have already been enumerated.\
 * Note: The 0x80 conversion it is looking might in theory block the message from being forwarded so it might be required
 * To do all of this differently. Try with multiple boards. The alternative method would be to try to set the next unused
 * address and see if any boards respond back saying that they set the address. 
 */
void setupBoards()
{
    uint8_t payload[3];
    uint8_t buff[10];

    payload[0] = 0;
    payload[1] = 0;
    payload[2] = 0;  
    while (1 == 1)
    {
        sendData(payload, 3, false);
        delay(3);
        if (getReply(buff) > 2)
        {
            if (buff[0] == 0x81 && buff[1] == 0 && buff[2] == 0)
            {
                //look for a free address to use
                for (int y = 0; y < 63; y++) 
                {
                    if (boards[y] == BS_MISSING)
                    {
                        payload[0] = 0;
                        payload[1] = REG_ADDR_CTRL;
                        payload[2] = y | 0x80;
                        sendData(payload, 3, true);
                        delay(3);
                        if (getReply(buff) > 2)
                        {
                            if (buff[0] == (y << 1) && buff[1] == REG_ADDR_CTRL && buff[2] == (y | 0x80)) boards[y] = BS_FOUND; //Success!
                        }
                        break; //quit the for loop
                    }
                }
            }
            else return; //nobody responded properly to the zero address so our work here is done.
        }
    }
}

/*
 * Iterate through all 63 possible board addresses (1-63) to see if they respond
 */
void findBoards()
{
    uint8_t payload[3];
    uint8_t buff[8];
    payload[1] = 0; //read registers starting at 0
    payload[2] = 1; //read one byte
    for (int x = 1; x < 64; x++) 
    {
        payload[0] = x << 1;
        sendData(payload, 3, false);
        delay(2);
        if (getReply(buff) > 4)
        {
            if (buff[0] == (x << 1) && buff[1] == 0 && buff[2] == 1) boards[x] = BS_FOUND;
            else boards[x] = BS_MISSING;
        }
    }
}

bool getModuleVoltage(uint8_t address)
{
    uint8_t payload[4];
    uint8_t buff[30];
    if (address < 1 || address > 63) return false;
    payload[0] = address << 1;
    payload[1] = REG_ADC_CTRL;
    payload[2] = 0b00111101; //ADC Auto mode, read every ADC input we can (Both Temps, Pack, 6 cells)
    sendData(payload, 3, true);
    delay(2);
    getReply(buff);
    payload[1] = REG_ADC_CONV;
    payload[2] = 1;
    sendData(payload, 3, true);
    delay(2);
    getReply(buff);
    payload[1] = REG_GPAI;
    payload[2] = 0x12; 
    sendData(payload, 3, false);
    delay(2);
    if (getReply(buff) > 0x13)
    {
        if (buff[0] == (address << 1) && buff[1] == REG_GPAI  && buff[2] == 0x12)
        {
            //payload is 2 bytes gpai, 2 bytes for each of 6 cell voltages, 2 bytes for each of two temperatures (18 bytes of data)
            moduleVolt[address - 1] = (buff[3] * 256 + buff[4]) * 0.002034609f;
            for (int i = 0; i < 6; i++) cellVolt[address - 1][i] = (buff[5 + (i * 2)] * 256 + buff[6 + (i * 2)]) * 0.000381493f;
            temperatures[address - 1][0] = (buff[17] * 256 + buff[18]);
            temperatures[address - 1][1] = (buff[19] * 256 + buff[20]);
            return true;
        }        
    }
    return false;
}

void setup() 
{
    delay(4000);
    Serial3.begin(612500);
    Serial.begin(115200);
    //serialSpecialInit(USART0, 612500);
    Serial.println("Fired up serial at 612500 baud!");
    for (int x = 0; x < 64; x++) boards[x] = BS_STARTUP;
    //setupBoards();
    findBoards();
    for (int x = 0; x < 64; x++) Serial.println(boards[x]);
     
}

void loop() 
{
    uint8_t payload[8];
    uint8_t buff[30];
    //payload[0] = 2;
    //payload[1] = 0x30;
    //payload[2] = 0b00111101;
    //sendData(payload, 3, true);
    //delay(3);
    //getReply(buff);
    delay(500);
    getModuleVoltage(1);
    Serial.println(moduleVolt[0]);
    Serial.println();
    for (int x = 0; x < 6; x++) 
    {
      Serial.print(cellVolt[0][x]); 
      Serial.print(", ");
    }
    Serial.println();
    Serial.print(temperatures[0][0]);
    Serial.print(", ");
    Serial.print(temperatures[0][1]);
    Serial.println();
   
    
}

