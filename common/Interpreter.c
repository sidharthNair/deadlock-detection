// *************Interpreter.c**************
// Students implement this as part of EE445M/EE380L.12 Lab 1,2,3,4
// High-level OS user interface
//
// Runs on LM4F120/TM4C123
// Jonathan W. Valvano 1/18/20, valvano@mail.utexas.edu
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../common/OS.h"
#include "../common/ST7735.h"
#include "../common/ADC.h"
#include "../common/UART0int.h"
#include "../common/eDisk.h"
#include "../common/eFile.h"
#include "../inc/ADCT0ATrigger.h"
#include "../inc/ADCSWTrigger.h"
#include "../deadlock/loader.h"

extern uint32_t DataLost;
extern int32_t MaxJitter;
extern uint32_t const JitterSize;
extern uint32_t JitterHistogram[];

extern uint32_t num_created;

extern uint32_t SumCritical;
extern uint32_t MaxCritical;
extern uint32_t start_time;

extern Sema4Type LCDFree;

static const ELFSymbol_t symbol_table[] = {
  { "ST7735_Message", ST7735_Message }
};

void OutCRLF(void){
  UART_OutChar(CR);
  UART_OutChar(LF);
}

void Jitter(int32_t MaxJitter, uint32_t const JitterSize, uint32_t JitterHistogram[], int device){
  // write this for Lab 3 (the latest)
  ST7735_Message(device, 0, "Jitter 0.1us=", MaxJitter);

  //print out number of times each jitter has been detected
  for(int i = 0; i < JitterSize; i++){
    UART_OutUDec(i);
    UART_OutString(" 0.1us=");
    UART_OutUDec(JitterHistogram[i]);
    for (int j = 0; j < JitterHistogram[i]; j++){
      UART_OutChar('*');
    }
    OutCRLF();
  }
}

#define BUFFER_LEN 64
#define MAX_TOKENS 4

int tokenize(char *input, char **tokens, int maxTokens) {
  int length = 0;
  *tokens = strtok(input, " ");
  while (*tokens && length < maxTokens) {
    tokens++;
    *tokens = strtok(NULL, " ");
    length++;
  }
  return length;
}

/* Possible commands style:
lcd_top str num
lcd_bottom str num
adc_in
clear_time*/


void executeCommand(char **tokens, int tokenCount) {
  if (tokenCount == 0) {
    // Empty command, do nothing
    return;
  }
  OutCRLF();
  char *command = tokens[0];

  if (strcmp(command, "lcd_top") == 0) {
    if (tokenCount >= 3) {
      int line = atoi(tokens[1]);
      char *str = tokens[2];
      int num = atoi(tokens[3]);
      ST7735_Message(0, line, str, num);
    } else {
      // Invalid lcd_top command
      UART_OutString("Invalid Syntax");
    }
  } else if (strcmp(command, "lcd_bottom") == 0) {
    if (tokenCount >= 3) {
      int line = atoi(tokens[1]);
      char *str = tokens[2];
      int num = atoi(tokens[3]);
      ST7735_Message(1, line, str, num);
    } else {
      UART_OutString("Invalid Syntax");
    }
  } else if (strcmp(command, "adc_in") == 0) {
    UART_OutString("ADC Data: ");
    uint32_t data = ADC_In();
    UART_OutUDec(data);
  } else if (strcmp(command, "clear_time") == 0) {
    OS_ClearMsTime();
  } else if (strcmp(command, "read_time") == 0) {
    uint32_t os_time = OS_MsTime();
    UART_OutUDec(os_time);
    UART_OutString(" ms");
  } else if (strcmp(command, "num_created") == 0) {
    UART_OutUDec(num_created);
    UART_OutString(" threads");
  } else if (strcmp(command, "max_jitter") == 0) {
    UART_OutUDec(MaxJitter);
    UART_OutString(" 0.1us");
  } else if (strcmp(command, "time_i_disabled") == 0) {
    UART_OutUDec((MaxCritical+4)/8);
    UART_OutString(" 0.1us");
  } else if (strcmp(command, "percent_i_disabled") == 0) {
    uint32_t total_time = OS_Time() - start_time;
    uint32_t percent = (10000 * SumCritical) / total_time;
    UART_OutUDec(percent);
    UART_OutString(" 0.01%");
  } else if (strcmp(command, "clear_i_disabled") == 0) {
    MaxCritical = 0;
    SumCritical = 0;
    start_time = OS_Time();
  } else if (strcmp(command, "ls") == 0) {
    char *name;
    unsigned long size = 0;
    unsigned int num = 0;
    unsigned long total = 0;
    if (eFile_DOpen("")) {
      printf("dir open error\r\n\r\n");
      return;
    }
    while (!eFile_DirNext(&name, &size)){
      printf("Filename = %s", name);
      printf("  ");
      printf("File size = %lu bytes", size);
      printf("\n\r");
      total = total+size;
      num++;
    }
    printf("Number of Files = %u\r\n", num);
    printf("Number of Bytes = %lu\r\n", total);
    if (eFile_DClose()) {
      printf("dir close error\r\n\r\n");
      return;
    }
  } else if (strcmp(command, "touch") == 0) {
    if (tokenCount >= 2) {
      if (eFile_Create(tokens[1])) {
        printf("file create error\r\n\r\n");
        return;
      }
    }
  } else if (strcmp(command, "echo") == 0) {
    if (tokenCount >= 3) {
      if (eFile_WOpen(tokens[2])) {
        printf("file open error\r\n\r\n");
        return;
      }
      char *ptr = tokens[1];
      while (*ptr) {
        if (eFile_Write(*(ptr++))) {
          printf("file write error\r\n\r\n");
          break;
        }
      }
      if (eFile_WClose()) {
        printf("file close error\r\n\r\n");
        return;
      }
    }
  } else if (strcmp(command, "cat") == 0) {
    if (tokenCount >= 2) {
      if (eFile_ROpen(tokens[1])) {
        printf("file open error\r\n\r\n");
        return;
      }
      char data;
      while (!eFile_ReadNext(&data)) {
        UART_OutChar(data);
      }
      printf("\r\n");
      if (eFile_RClose()) {
        printf("file close error\r\n\r\n");
        return;
      }
    }
  } else if (strcmp(command, "rm") == 0) {
    if (tokenCount >= 2) {
      if (eFile_Delete(tokens[1])) {
        printf("file remove error\r\n\r\n");
        return;
      }
    }
   } else if (strcmp(command, "format") == 0) {
    if (eFile_Format()) {
      printf("format error\r\n\r\n");
      return;
    }
  } else if (strcmp(command, "unmount") == 0) {
    if (eFile_Unmount()) {
      printf("unmount error\r\n\r\n");
      return;
    }
  } else if (strcmp(command, "load_elf") == 0) {
    if (tokenCount >= 2) {
      ELFEnv_t env = { symbol_table, 1 };
      OS_bWait(&LCDFree);
      int result = exec_elf(tokens[1], &env);
      OS_bSignal(&LCDFree);
      if (result == 1) {
        printf("load successful\r\n");
      } else {
        printf("load error %d\r\n", result);
      }
    }
  } else {
    // Unknown command
    UART_OutString("Unknown Command");
  }
  OutCRLF();
}

// *********** Command line interpreter (shell) ************
void Interpreter(void){
  if(eFile_Init()) {
    printf("Error initializing file system.\n\r");
  }
  // mount the file system
  if(eFile_Mount()) {
    printf("Error mounting file system.\n\r");
  }

  // write this
  char *tokens[MAX_TOKENS];
  char command[BUFFER_LEN];

  tokens[0] = "ls";
  executeCommand(tokens, 1);
  while (TRUE) {
    // TODO: Print Menu
    OutCRLF();
    UART_OutString("Command List:\r\n");
    UART_OutString("lcd_top [line] [str] [num]\r\n");
    UART_OutString("lcd_bottom [line] [str] [num]\r\n");
    UART_OutString("adc_in\r\n");
    UART_OutString("read_time\r\n");
    UART_OutString("clear_time\r\n");
    UART_OutString("num_created\r\n");
    UART_OutString("max_jitter\r\n");
    UART_OutString("time_i_disabled\r\n");
    UART_OutString("percent_i_disabled\r\n");
    UART_OutString("clear_i_disabled\r\n");
    UART_OutString("ls\r\n");
    UART_OutString("touch [file_name]\r\n");
    UART_OutString("echo [str] [file_name]\r\n");
    UART_OutString("cat [file_name]\r\n");
    UART_OutString("rm [file_name]\r\n");
    UART_OutString("format\r\n");
    UART_OutString("unmount\r\n");
    UART_OutString("load_elf [file_name]\r\n");
    OutCRLF();
    UART_InString(command, BUFFER_LEN);
    int tokenCount = tokenize(command, tokens, MAX_TOKENS);
    executeCommand(tokens, tokenCount);
  }
}
