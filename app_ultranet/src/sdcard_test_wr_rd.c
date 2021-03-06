// Copyright (c) 2011, XMOS Ltd., All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

/*
 ============================================================================
 Name        : sdcard_test
 Description : SD card host driver test
 ============================================================================
 */

#include <xccompat.h>       // Enable XMOS streaming channel types to pass through.  Need this before other header files
#define streaming

#include <stdio.h> /* for the printf function */
#include "ff.h"    /* file system routines */
#include "timing.h"
#include "diskio.h"     /* To get the bus mode definition for debugging */
#include "string.h"     /* To get memset function */
#include <stdint.h>

FATFS Fatfs;            /* File system object */
FIL Fil;                /* File object */
// BYTE Buff[512*1];      /* File read buffer (Make Smaller temporary/ at least 32 SD card blocks to let multiblock operations (if file not fragmented) */
#define max(a,b) ((a)>(b))?(a):(b)
#define min(a,b) ((a)<(b))?(a):(b)

void die(FRESULT rc ) /* Stop with dying message */
{
  printf("\nFailed with rc=%u.\n", rc);
  for(;;);
}

// Readback testing functions
void init_the_crc(unsigned *p);
void walk_the_crc(unsigned *p);

// Implemented in xs1.h
void crc32(unsigned *checksum, unsigned data, unsigned poly);
// Generate predictable pseudo-random traffic (that we can compare against for proof of testing)
#define CRC32_ETH_REV_POLY 0xEDB88320       // x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x^1 + x^0
                                            // See https://github.com/xcore/doc_tips_and_tricks/blob/master/doc/crc.rst

DWORD allocate_contiguous_clusters (    /* Returns the first sector in LBA (0:error or not contiguous) */
    FIL* fp,    /* Pointer to the open file object */
    DWORD len   /* Number of bytes to allocate */
);

DRESULT disk_write_streamed(BYTE IfNum, streaming chanend c, DWORD sector, UINT count);

extern uint32_t SendCmd_twr_max, SendCmd_twr_min, SendCmd_twr_total, SendCmd_twr_count;


/*****************************************************************************************/

void disk_write_read_task(chanend c, uint32_t targetFileSize)
{
  FRESULT rc;                     /* Result code */
  DIR dir;                        /* Directory object */
  FILINFO fno;                    /* File information object */
  UINT T;

  if(targetFileSize & 511) {
      printf("Error: file size not exact number of clusters\n");
      die(0);
  }
  f_mount(&Fatfs, "", 1);        // Register volume work area (never fails) for SD host interface #0
                                 // Note the params have changed between fatFS 0.09 and 0.11

  rc = f_unlink("DATA.WAV");     /* delete file if exist */

  rc = f_open(&Fil, "DATA.WAV", FA_WRITE | FA_CREATE_ALWAYS);
  if(rc) die(rc);

  // XMOS Streamed I/O and direct disk write
  // Pre-allocate clusters to the file
  printf("Expected file size %lu bytes (0 means 4G!) \n", targetFileSize);
  T = get_time();
  DWORD fileSect = allocate_contiguous_clusters(&Fil, targetFileSize);
  unsigned alloc_time = get_time()-T;
  printf("Allocation took %u ms\n", alloc_time/100000);
  if (!fileSect) {              // Allocation failed
      printf("Error: allocate_contiguous_clusters() failed.\n");
      f_close(&Fil);
      die(0);
  }
  else {
      /*
       * This is the main Write call - this does not return until the file is done
       */
      printf("Syncing the file system\n");
      rc = f_sync(&Fil);        // Ensure FAT info is written for this file
      if(rc) die(rc);

      printf("Streaming directly to the file... \n");
      DWORD sectorsPerWriteChunk = Fatfs.csize*16;          // We will attempt to write this many
      DWORD bytesPerWriteChunk = sectorsPerWriteChunk<<9;   // Assume 512-byte sectors

      while(targetFileSize) {
          if(targetFileSize < bytesPerWriteChunk) {
              // Partial streamed write chunk at end
              sectorsPerWriteChunk = targetFileSize>>9;     // Assume 512-byte sectors
              bytesPerWriteChunk = targetFileSize;          // End of file can be of arbitrary length, but we'll always round down to whole sector
          }
          //printf("Writing sector %lu with %lu sectors, %lu bytes\n", fileSect, sectorsPerWriteChunk, bytesPerWriteChunk);

          // Instrumentation:  Measure max/min response times
          SendCmd_twr_max = 0;
          SendCmd_twr_min = 9999999;
          SendCmd_twr_total = 0;
          SendCmd_twr_count = 0;

          rc = disk_write_streamed(Fil.fs->drv, c, fileSect, sectorsPerWriteChunk);
          if(rc) die(rc);
          //printf("SendCmd took max %lu usec, min %lu usec, total %lu usec, count %lu, avg %lu usec\n",
          //        SendCmd_twr_max/100, SendCmd_twr_min/100, SendCmd_twr_total, SendCmd_twr_count, SendCmd_twr_total/SendCmd_twr_count);

          targetFileSize -= bytesPerWriteChunk;             // Update the amount written so far, and the working sector number
          fileSect += sectorsPerWriteChunk;
      }
  }

  printf("\nClosing the file...");
  rc = f_close(&Fil);
  if(rc) die(rc);
  printf("done.\n");

  /****************************/
#if 0
  printf("\nOpening an existing file: Data.bin...");
  rc = f_open(&Fil, "Data.bin", FA_READ);
  if(rc) die(rc);
  printf("done.\n");

  printf("\nReading file content...\n");

  unsigned filepos = 0;
  while(!f_eof(&Fil)) {
    unsigned br;
    memset(Buff, 0, sizeof(Buff));
    rc = f_read(&Fil, Buff, sizeof(Buff), &br);
    if(rc) die(rc);

    // Check the read back contents of the buffer
    unsigned *lBuff;
    lBuff = (unsigned *)&Buff;               // Cast the pointer so we read & check unsigned 32-bit values
    for(j=0; j<br/sizeof(signed); j++) {
        if(j%16 ==0) printf("\n%08x: ", filepos);
        printf("%08x ", lBuff[j]);
        filepos++;
    }
  }

  printf("\nClosing the file...");
  rc = f_close(&Fil);
  if(rc) die(rc);
  printf("done.\n");

  /****************************/
#endif
  printf("\nOpen root directory.\n");
  rc = f_opendir(&dir, "");
  if(rc) die(rc);

  printf("\nDirectory listing...\n");
  for(;;)
  {
    rc = f_readdir(&dir, &fno);    /* Read a directory item */
    if(rc || !fno.fname[0]) break; /* Error or end of dir */
    if(fno.fattrib & AM_DIR)
      printf("   <dir>  %s\n", fno.fname);
    else
    {
      printf("%8lu  %s\n", fno.fsize, fno.fname);
    }
  }
  if(rc) die(rc);

  /****************************/

  printf("\nTest completed.\n");
}




