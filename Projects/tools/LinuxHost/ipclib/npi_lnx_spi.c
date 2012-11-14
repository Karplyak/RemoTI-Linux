/**************************************************************************************************
  Filename:       npi_lnx_spi.c
  Revised:        $Date: 2012-03-15 13:45:31 -0700 (Thu, 15 Mar 2012) $
  Revision:       $Revision: 237 $

  Description:    This file contains linux specific implementation of Network Processor Interface
                  module.


  Copyright (C) {2012} Texas Instruments Incorporated - http://www.ti.com/


   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

     Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.

     Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the
     distribution.

     Neither the name of Texas Instruments Incorporated nor the names of
     its contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**************************************************************************************************/

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <linux/input.h>
#include <poll.h>

#include "aic.h"
#include "npi_lnx.h"
#include "npi_lnx_spi.h"
#include "hal_rpc.h"
#include "hal_gpio.h"

#include "npi_lnx_error.h"

#ifdef __STRESS_TEST__
#include <sys/time.h>
#elif (defined __DEBUG_TIME__)
#include <sys/time.h>
#else
#include <sys/time.h>
#endif // __STRESS_TEST__

// -- macros --

#ifndef TRUE
# define TRUE (1)
#endif

#ifndef FALSE
# define FALSE (0)
#endif

#ifdef __BIG_DEBUG__
#define debug_printf(fmt, ...) printf( fmt, ##__VA_ARGS__)
#else
#define debug_printf(fmt, ...) st (if (__BIG_DEBUG_ACTIVE == TRUE) printf( fmt, ##__VA_ARGS__);)
#endif

// -- Constants --

// -- Local Variables --

// State variable used to indicate that a device is open.
static int npiOpenFlag = FALSE;

// NPI device related variables
static int              npi_poll_terminate;
static pthread_mutex_t  npiPollLock;
static pthread_mutex_t  npi_poll_mutex;
static int 				GpioSrdyFd;
#ifndef SRDY_INTERRUPT
static pthread_cond_t   npi_poll_cond;
#endif

// Polling thread
//---------------
static pthread_t        npiPollThread;
static int     			PollLockVar = 0;

// thread subroutines
static void npi_termpoll(void);
static void *npi_poll_entry(void *ptr);


#ifdef SRDY_INTERRUPT
// Event thread
//---------------
static pthread_t        npiEventThread;
static void *npi_event_entry(void *ptr);
static int global_srdy;

static pthread_cond_t   npi_srdy_H2L_poll;

static pthread_mutex_t  npiSrdyLock;
#define INIT 0
#define READY 1
#endif

// -- Forward references of local functions --

// -- Public functions --

#ifdef __STRESS_TEST__
extern struct timeval curTime, startTime;
struct timeval prevTimeI2C;
#elif (defined __DEBUG_TIME__)
struct timeval curTime, prevTime, startTime;
#else
struct timeval curTime, prevTime;
#endif //__STRESS_TEST__

// -- Private functions --
static int npi_initsyncres(void);
static int npi_initThreads(void);

/******************************************************************************
 * @fn         PollLockVarError
 *
 * @brief      This function kill the program due to a major Mutex problem.
 *
 * input parameters
 *
 * None.
 *
 * output parameters
 *
 * None.
 *
 * @return
 *
 * None.
 ******************************************************************************
 */
int PollLockVarError(void)
{
    printf("PollLock Var ERROR, it is %d, it should be %d", PollLockVar, !PollLockVar);
    npi_ipc_errno = NPI_LNX_ERROR_SPI_POLL_LOCK_VAR_ERROR;
    return NPI_LNX_FAILURE;
}
/******************************************************************************
 * @fn         NPI_OpenDevice
 *
 * @brief      This function establishes a serial communication connection with
 *             a network processor device.
 *             As windows machine does not have a single dedicated serial
 *             interface, this function will designate which serial port shall
 *             be used for communication.
 *
 * input parameters
 *
 * @param   portName 	– name of the serial port
 * @param	gpioCfg		– GPIO settings for SRDY, MRDY and RESET
 *
 * output parameters
 *
 * None.
 *
 * @return     TRUE if the connection is established successfully.
 *             FALSE, otherwise.
 ******************************************************************************
 */
int NPI_SPI_OpenDevice(const char *portName, void *pCfg)
{
	int ret = NPI_LNX_SUCCESS;

  if(npiOpenFlag)
  {
	  npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_ALREADY_OPEN;
	  return NPI_LNX_FAILURE;
  }

#ifdef __DEBUG_TIME__
  gettimeofday(&startTime, NULL);
#endif //__DEBUG_TIME__

  npiOpenFlag = TRUE;

  debug_printf("Opening Device File: %s\n", portName);

  ret = HalSpiInit(portName, ((npiSpiCfg_t*)pCfg)->speed);
  if (ret != NPI_LNX_SUCCESS)
	return ret;

  debug_printf("((npiSpiCfg_t *)pCfg)->gpioCfg[0] \t @0x%.8X\n",
		  (unsigned int)&(((npiSpiCfg_t *)pCfg)->gpioCfg[0]));

  if ( NPI_LNX_FAILURE == (GpioSrdyFd = HalGpioSrdyInit(((npiSpiCfg_t *)pCfg)->gpioCfg[0])))
	return GpioSrdyFd;
  if ( NPI_LNX_FAILURE == (ret = HalGpioMrdyInit(((npiSpiCfg_t *)pCfg)->gpioCfg[1])))
	return ret;
  if ( NPI_LNX_FAILURE == (ret = HalGpioResetInit(((npiSpiCfg_t *)pCfg)->gpioCfg[2])))
	return ret;

  // initialize thread synchronization resources
  if ( NPI_LNX_FAILURE == (ret = npi_initsyncres()))
	return ret;


  //Polling forbid until the Reset and Sync is done
  debug_printf("LOCK POLL WHILE INIT\n");
  pthread_mutex_lock(&npiPollLock);
  if (PollLockVar)
	  ret = PollLockVarError();
  else
	  PollLockVar = 1;

  debug_printf("PollLockVar = %d\n", PollLockVar);

  // TODO: it is ideal to make this thread higher priority
  // but Linux does not allow real time of FIFO scheduling policy for
  // non-privileged threads.

  if (ret == NPI_LNX_SUCCESS)
	  // create Polling thread
	  ret = npi_initThreads();
  else
	  debug_printf("Did not attempt to start Threads\n");

  return ret;
}


/******************************************************************************
 * @fn         NPI_SPI_CloseDevice
 *
 * @brief      This function closes connection with a network processor device
 *
 * input parameters
 *
 * @param      pDevice   - pointer to a device data structure
 *
 * output parameters
 *
 * None.
 *
 * @return     None
 ******************************************************************************
 */
void NPI_SPI_CloseDevice(void)
{
  npi_termpoll();
  HalSpiClose();
  HalGpioSrdyClose();
  HalGpioMrdyClose();
  HalGpioResetClose();
  npiOpenFlag = FALSE;
}

/**************************************************************************************************
 * @fn          NPI_SPI_SendAsynchData
 *
 * @brief       This function is called by the client when it has data ready to
 *              be sent asynchronously. This routine allocates an AREQ buffer,
 *              copies the client's payload, and sets up the send.
 *
 * input parameters
 *
 * @param *pMsg  - Pointer to data to be sent asynchronously (i.e. AREQ).
 *
 * output parameters
 *
 * None.
 *
 * @return      STATUS
 **************************************************************************************************
 */
int NPI_SPI_SendAsynchData( npiMsgData_t *pMsg )
{
	int ret = NPI_LNX_SUCCESS;
	debug_printf("Sync Lock SRDY ...");
	fflush(stdout);
	//Lock the polling until the command is send
	pthread_mutex_lock(&npiPollLock);
#ifdef SRDY_INTERRUPT
	pthread_mutex_lock(&npiSrdyLock);
#endif
	if (PollLockVar)
		ret = PollLockVarError();
	else
		PollLockVar = 1;
	debug_printf("(Sync) success \n");

	debug_printf("\n******************** START SEND ASYNC DATA ********************\n");
	// Add Proper RPC type to header
	((uint8*)pMsg)[RPC_POS_CMD0] = (((uint8*)pMsg)[RPC_POS_CMD0] & RPC_SUBSYSTEM_MASK) | RPC_CMD_AREQ;

	if (ret == NPI_LNX_SUCCESS)
		if ( NPI_LNX_SUCCESS != (ret = HAL_RNP_MRDY_CLR()))
			return ret;

	debug_printf("[AREQ]");

	//Wait for SRDY Clear
	ret = HalGpioWaitSrdyClr();

	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, (uint8*) pMsg, (pMsg->len)+RPC_FRAME_HDR_SZ);

	if (ret == NPI_LNX_SUCCESS)
		ret = HAL_RNP_MRDY_SET();
	else
		(void)HAL_RNP_MRDY_SET();

	if (!PollLockVar)
		ret = PollLockVarError();
	else
		PollLockVar = 0;
	pthread_mutex_unlock(&npiPollLock);
#ifdef SRDY_INTERRUPT
	pthread_mutex_unlock(&npiSrdyLock);
#endif
	debug_printf("Sync unLock SRDY ...\n\n");
	debug_printf("\n******************** STOP SEND ASYNC DATA ********************\n");

	return ret;
}

/**************************************************************************************************
 * @fn          npi_spi_pollData
 *
 * @brief       This function is called by the client when it has data ready to
 *              be sent synchronously. This routine allocates a SREQ buffer,
 *              copies the client's payload, sends the data, and waits for the
 *              reply. The input buffer is used for the output data.
 *
 * input parameters
 *
 * @param *pMsg  - Pointer to data to be sent synchronously (i.e. the SREQ).
 *
 * output parameters
 *
 * @param *pMsg  - Pointer to replay data (i.e. the SRSP).
 *
 * @return      STATUS
 **************************************************************************************************
 */
int npi_spi_pollData(npiMsgData_t *pMsg)
{
	int i, ret = NPI_LNX_SUCCESS;
#ifdef SRDY_INTERRUPT
	pthread_mutex_lock(&npiSrdyLock);
#endif
	debug_printf("\n-------------------- START POLLING DATA --------------------\n");

#ifdef __BIG_DEBUG__
	printf("Polling Command ...");

	for(i = 0 ; i < (RPC_FRAME_HDR_SZ+pMsg->len); i++)
		printf(" 0x%.2x", ((uint8*)pMsg)[i]);

	printf("\n");
#endif

#ifdef __STRESS_TEST__
	//	debug_
	gettimeofday(&curTime, NULL);
	long int diffPrev;
	int t = 0;
	if (curTime.tv_usec >= prevTimeI2C.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTimeI2C.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTimeI2C.tv_usec;
		t = 1;
	}

	prevTimeI2C = curTime;

	printf("[--> %.5ld.%.6ld (+%ld.%6ld)] MRDY Low \n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTimeI2C.tv_sec - t,
			diffPrev);
#endif //__STRESS_TEST__

	if (ret == NPI_LNX_SUCCESS)
		if ( NPI_LNX_SUCCESS != (ret = HAL_RNP_MRDY_CLR()))
			return ret;

	ret = HalSpiWrite( 0, (uint8*) pMsg, (pMsg->len)+RPC_FRAME_HDR_SZ);

	struct timeval t1, t2;

	gettimeofday(&t1, NULL);
#if __BIG_DEBUG__
	printf("[POLL] %.5ld.%.6ld]\n", t1.tv_sec, t1.tv_usec);
#endif

	//Wait for SRDY set
	if (ret == NPI_LNX_SUCCESS)
		ret = HalGpioWaitSrdySet();

	// Check how long it took to wait for SRDY to go High. May indicate that this Poll was considered
	// a handshake by the RNP.
	gettimeofday(&t2, NULL);
	debug_printf("[POLL] %.5ld.%.6ld]\n", t2.tv_sec, t2.tv_usec);
	long int diffPrev;
	if (t2.tv_usec >= t1.tv_usec)
	{
		diffPrev = t2.tv_usec - t1.tv_usec;
		diffPrev += (t2.tv_sec - t1.tv_sec) * 1000000;
	}
	else
	{
		diffPrev = (t2.tv_usec + 1000000) - t1.tv_usec;
		diffPrev += (t2.tv_sec - t1.tv_sec - 1) * 1000000;
	}

	// If it took more than 100ms then it's likely a reset handshake.
	if (diffPrev > (100 * 1000) )
	{
		debug_printf("[POLL] SRDY took %ld us to go high\n", diffPrev);
		npi_ipc_errno = NPI_LNX_ERROR_SPI_POLL_DATA_SRDY_CLR_TIMEOUT_POSSIBLE_RESET;
		return NPI_LNX_FAILURE;
	}

	//We Set MRDY here to avoid GPIO latency with the beagle board
	// if we do here later, the RNP see it low at the end of the transaction and
	// therefore think a new transaction is starting and lower its SRDY...
	if (ret == NPI_LNX_SUCCESS)
		ret = HAL_RNP_MRDY_SET();
	else
		(void)HAL_RNP_MRDY_SET();

	//Do a Three Byte Dummy Write to read the RPC Header
	for (i = 0 ;i < RPC_FRAME_HDR_SZ; i++ ) ((uint8*)pMsg)[i] = 0;
	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, (uint8*) pMsg, RPC_FRAME_HDR_SZ);


	//Do a write/read of the corresponding length
	for (i = 0 ;i < ((uint8*)pMsg)[0]; i++ ) ((uint8*)pMsg)[i+RPC_FRAME_HDR_SZ] = 0;
	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, pMsg->pData, ((uint8*)pMsg)[0]);

#ifdef __BIG_DEBUG__
	if (TRUE == HAL_RNP_SRDY_CLR())
		printf("SRDY set\n");
	else
		printf("SRDY Clear\n");
#endif

#ifdef __BIG_DEBUG__
	printf("Poll Response Received ...");
	for (i = 0 ; i < (RPC_FRAME_HDR_SZ+pMsg->len); i++ ) printf(" 0x%.2x", ((uint8*)pMsg)[i]);
	printf("\n");
#endif
	debug_printf("\n-------------------- END POLLING DATA --------------------\n");
#ifdef SRDY_INTERRUPT
	pthread_mutex_unlock(&npiSrdyLock);
#endif

	return ret;
}

/**************************************************************************************************
 * @fn          NPI_SPI_SendSynchData
 *
 * @brief       This function is called by the client when it has data ready to
 *              be sent synchronously. This routine allocates a SREQ buffer,
 *              copies the client's payload, sends the data, and waits for the
 *              reply. The input buffer is used for the output data.
 *
 * input parameters
 *
 * @param *pMsg  - Pointer to data to be sent synchronously (i.e. the SREQ).
 *
 * output parameters
 *
 * @param *pMsg  - Pointer to replay data (i.e. the SRSP).
 *
 * @return      STATUS
 **************************************************************************************************
 */
int NPI_SPI_SendSynchData( npiMsgData_t *pMsg )
{
	int i, ret = NPI_LNX_SUCCESS;
	// Do not attempt to send until polling is finished

	int lockRet = 0;
	debug_printf("\nSync Lock SRDY ...");
	fflush(stdout);
	//Lock the polling until the command is send
	lockRet = pthread_mutex_lock(&npiPollLock);
#ifdef SRDY_INTERRUPT
	pthread_mutex_lock(&npiSrdyLock);
#endif
	if (PollLockVar)
		ret = PollLockVarError();
	else
		PollLockVar = 1;
	debug_printf("(Sync) success \n");
	debug_printf("==================== START SEND SYNC DATA ====================\n");
	if (lockRet != 0)
	{
		printf("[ERR] Could not get lock\n");
		perror("mutex lock");
	}

	// Add Proper RPC type to header
	((uint8*)pMsg)[RPC_POS_CMD0] = (((uint8*)pMsg)[RPC_POS_CMD0] & RPC_SUBSYSTEM_MASK) | RPC_CMD_SREQ;


#ifdef __BIG_DEBUG__
	if (TRUE == HAL_RNP_SRDY_CLR())
		printf("SRDY set\n");
	else
		printf("SRDY Clear\n");
#endif

#ifdef __BIG_DEBUG__
	printf("Sync Data Command ...");
	for (i = 0 ; i < (RPC_FRAME_HDR_SZ+pMsg->len); i++ ) printf(" 0x%.2x", ((uint8*)pMsg)[i]);
	printf("\n");
#endif

#ifdef __STRESS_TEST__
	//	debug_
	gettimeofday(&curTime, NULL);
	long int diffPrev;
	int t = 0;
	if (curTime.tv_usec >= prevTimeI2C.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTimeI2C.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTimeI2C.tv_usec;
		t = 1;
	}

	prevTimeI2C = curTime;

	printf("[--> %.5ld.%.6ld (+%ld.%6ld)] MRDY Low \n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTimeI2C.tv_sec - t,
			diffPrev);
#endif //__STRESS_TEST__

	if (ret == NPI_LNX_SUCCESS)
		if ( NPI_LNX_SUCCESS != (ret = HAL_RNP_MRDY_CLR()))
			return ret;

	//Wait for SRDY Clear
	ret = HalGpioWaitSrdyClr();

	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, (uint8*) pMsg, (pMsg->len)+RPC_FRAME_HDR_SZ);

	debug_printf("[SREQ]");
	//Wait for SRDY set
	if (ret == NPI_LNX_SUCCESS)
		ret = HalGpioWaitSrdySet();

	//We Set MRDY here to avoid GPIO latency with the beagle board
	// if we do here later, the RNP see it low at the end of the transaction and
	// therefore think a new transaction is starting and lower its SRDY...
	if (ret == NPI_LNX_SUCCESS)
		ret = HAL_RNP_MRDY_SET();

	//Do a Three Byte Dummy Write to read the RPC Header
	for (i = 0 ;i < RPC_FRAME_HDR_SZ; i++ ) ((uint8*)pMsg)[i] = 0;
	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, (uint8*) pMsg, RPC_FRAME_HDR_SZ);


	//Do a write/read of the corresponding length
	for (i = 0 ;i < ((uint8*)pMsg)[0]; i++ ) ((uint8*)pMsg)[i+RPC_FRAME_HDR_SZ] = 0;
	if (ret == NPI_LNX_SUCCESS)
		ret = HalSpiWrite( 0, pMsg->pData, ((uint8*)pMsg)[0]);

	//End of transaction
	if (ret == NPI_LNX_SUCCESS)
		ret = HAL_RNP_MRDY_SET();
	else
		(void)HAL_RNP_MRDY_SET();

#ifdef __BIG_DEBUG__
	if (TRUE == HAL_RNP_SRDY_CLR())
		printf("SRDY set\n");
	else
		printf("SRDY Clear\n");
#endif

#ifdef __BIG_DEBUG__
	printf("Sync Data Receive ...");
	for (i = 0 ; i < (RPC_FRAME_HDR_SZ+pMsg->len); i++ ) printf(" 0x%.2x", ((uint8*)pMsg)[i]);
	printf("\n");
#endif

	//Release the polling lock
	//This is the SRSP, clear out the PC type in header
	((uint8 *)pMsg)[RPC_POS_CMD0] &=  RPC_SUBSYSTEM_MASK;

    debug_printf("\n==================== END SEND SYNC DATA ====================\n");
	if (!PollLockVar)
		ret = PollLockVarError();
	else
		PollLockVar = 0;
	pthread_mutex_unlock(&npiPollLock);
#ifdef SRDY_INTERRUPT
    pthread_mutex_unlock(&npiSrdyLock);
#endif
    debug_printf("Sync unLock SRDY ...\n\n");

	return ret;
}


/**************************************************************************************************
 * @fn          NPI_SPI_ResetSlave
 *
 * @brief       do the HW synchronization between the host and the RNP
 *
 * input parameters
 *
 * @param      none
 *
 * output parameters
 *
 * None.
 *
 * @return      STATUS
 **************************************************************************************************
 */
int NPI_SPI_ResetSlave( void )
{
	int ret = NPI_LNX_SUCCESS;

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	long int diffPrev;
	int t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] ----- START RESET SLAVE ------------\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#else //(!defined __DEBUG_TIME__)
  printf("\n\n-------------------- START RESET SLAVE -------------------\n");
#endif //(defined __DEBUG_TIME__)

  ret = HalGpioReset();

  printf("Wait 500us for RNP to initialize after a Reset... This may change in the future, check for RTI_ResetInd()...\n");
  usleep(500); //wait 500us for RNP to initialize

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] ----- END RESET SLAVE --------------\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#else //(!defined __DEBUG_TIME__)
  printf("-------------------- END RESET SLAVE -------------------\n");
#endif //(defined __DEBUG_TIME__)

  return ret;
}

/* Initialize thread synchronization resources */
static int npi_initThreads(void)
{
	int ret = NPI_LNX_SUCCESS;
	// create Polling thread
  // initialize SPI receive thread related variables
	npi_poll_terminate = 0;

	// TODO: it is ideal to make this thread higher priority
	// but linux does not allow realtime of FIFO scheduling policy for
	// non-priviledged threads.

	if(pthread_create(&npiPollThread, NULL, npi_poll_entry, NULL))
	{
		// thread creation failed
		NPI_SPI_CloseDevice();
    	npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_POLL_THREAD;
    	return NPI_LNX_FAILURE;
	}
#ifdef SRDY_INTERRUPT

	if(pthread_create(&npiEventThread, NULL, npi_event_entry, NULL))
	{
		// thread creation failed
		NPI_SPI_CloseDevice();
    	npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_EVENT_THREAD;
    	return NPI_LNX_FAILURE;
	}
#endif

	return ret;

}

/**************************************************************************************************
 * @fn          NPI_SPI_SynchSlave
 *
 * @brief       do the HW synchronization between the host and the RNP
 *
 * input parameters
 *
 * @param      none
 *
 * output parameters
 *
 * None.
 *
 * @return      STATUS
 **************************************************************************************************
 */
int NPI_SPI_SynchSlave( void )
{
	int ret = NPI_LNX_SUCCESS;
   printf("\n\n-------------------- START GPIO HANDSHAKE -------------------\n");

#ifdef SRDY_INTERRUPT
	pthread_mutex_lock(&npiSrdyLock);
#endif

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	long int diffPrev;
	int t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] Handshake Lock SRDY... Wait for SRDY to go Low\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#else //(!defined __DEBUG_TIME__)
  printf("Handshake Lock SRDY ...\n");
#endif // defined __DEBUG_TIME__

  // Check that SRDY is low
  ret = HalGpioWaitSrdyClr();

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] Set MRDY Low\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#endif // defined __DEBUG_TIME__

  // set MRDY to Low
	if (ret == NPI_LNX_SUCCESS)
		if ( NPI_LNX_SUCCESS != (ret = HAL_RNP_MRDY_CLR()))
			return ret;

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] Wait for SRDY to go High\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#endif // defined __DEBUG_TIME__

  // Wait for SRDY to go High
	ret = HalGpioWaitSrdySet();

#ifdef __DEBUG_TIME__
	gettimeofday(&curTime, NULL);
	t = 0;
	if (curTime.tv_usec >= prevTime.tv_usec)
	{
		diffPrev = curTime.tv_usec - prevTime.tv_usec;
	}
	else
	{
		diffPrev = (curTime.tv_usec + 1000000) - prevTime.tv_usec;
		t = 1;
	}

	prevTime = curTime;

	//	debug_
	printf("[%.5ld.%.6ld (+%ld.%6ld)] Set MRDY High\n",
			curTime.tv_sec - startTime.tv_sec,
			curTime.tv_usec,
			curTime.tv_sec - prevTime.tv_sec - t,
			diffPrev);
#endif // defined __DEBUG_TIME__
  // Set MRDY to High
	if (ret == NPI_LNX_SUCCESS)
		ret = HAL_RNP_MRDY_SET();
	else
		(void)HAL_RNP_MRDY_SET();

  	if (ret == NPI_LNX_SUCCESS)
		ret = HalGpioSrdyCheck(1);

  if (!PollLockVar)
	  ret = PollLockVarError();
  else
	  PollLockVar = 0;

  pthread_mutex_unlock(&npiPollLock);
	printf("Handshake unLock Poll ...");
  printf("(Handshake) success \n");
#ifdef SRDY_INTERRUPT
	pthread_mutex_unlock(&npiSrdyLock);
#endif
  printf("-------------------- END GPIO HANDSHAKE -------------------\n");

  return ret;
}


/**************************************************************************************************
 * @fn          npi_initsyncres
 *
 * @brief       Thread initialization
 *
 * input parameters
 *
 * @param      none
 *
 * output parameters
 *
 * None.
 *
 * @return      None.
 **************************************************************************************************
 */
static int npi_initsyncres(void)
{
  // initialize all mutexes
	debug_printf("LOCK POLL CREATED\n");
  if (pthread_mutex_init(&npiPollLock, NULL))
  {
    printf("Fail To Initialize Mutex npiPollLock\n");
    npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_POLL_LOCK_MUTEX;
    return NPI_LNX_FAILURE;
  }

  if(pthread_mutex_init(&npi_poll_mutex, NULL))
  {
    printf("Fail To Initialize Mutex npi_poll_mutex\n");
    npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_POLL_MUTEX;
    return NPI_LNX_FAILURE;
  }
#ifdef SRDY_INTERRUPT
	if(pthread_cond_init(&npi_srdy_H2L_poll, NULL))
	{
		printf("Fail To Initialize Condition npi_srdy_H2L_poll\n");
	    npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_SRDY_COND;
	    return NPI_LNX_FAILURE;
	}
	if (pthread_mutex_init(&npiSrdyLock, NULL))
	{
		printf("Fail To Initialize Mutex npiSrdyLock\n");
	    npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_SRDY_LOCK_MUTEX;
	    return NPI_LNX_FAILURE;
	}
#else
  if(pthread_cond_init(&npi_poll_cond, NULL))
  {
    printf("Fail To Initialize Condition npi_poll_cond\n");
    npi_ipc_errno = NPI_LNX_ERROR_SPI_OPEN_FAILED_POLL_COND;
    return NPI_LNX_FAILURE;
  }
#endif
  return NPI_LNX_SUCCESS;
}


/**************************************************************************************************
 * @fn          npi_poll_entry
 *
 * @brief       Poll Thread entry function
 *
 * input parameters
 *
 * @param      ptr
 *
 * output parameters
 *
 * None.
 *
 * @return      None.
 **************************************************************************************************
 */
static void *npi_poll_entry(void *ptr)
{
	int ret = NPI_LNX_SUCCESS;
	uint8 readbuf[128];
	uint8 pollStatus = FALSE;

	printf("POLL: Locking Mutex for Poll Thread \n");

	/* lock mutex in order not to lose signal */
	pthread_mutex_lock(&npi_poll_mutex);

	printf("POLL: Thread Started \n");

	//This lock wait for Initialization to finish (reset+sync)
	pthread_mutex_lock(&npiPollLock);

	printf("POLL: Thread Continues After Synchronization\n");

#ifdef SRDY_INTERRUPT
	debug_printf("POLL: Lock Poll mutex (SRDY=%d) \n", global_srdy);
	pthread_cond_wait(&npi_srdy_H2L_poll, &npiPollLock);
	debug_printf("POLL: Locked Poll mutex (SRDY=%d) \n", global_srdy);
#else
	pthread_mutex_unlock(&npiPollLock);
#endif

	/* thread loop */
	while(!npi_poll_terminate)
	{

#ifndef SRDY_INTERRUPT
		pthread_mutex_lock(&npiPollLock);
#endif
		if (PollLockVar)
			ret = PollLockVarError();
		else
			PollLockVar = 1;

		debug_printf("(Poll) success \n");
		//Ready SRDY Status
		// This Test check if RNP has asserted SRDY line because it has some Data pending.
		// If SRDY is not Used, then this line need to be commented, and the Poll command need
		// to be sent regularly to check if any data is pending. this is done every 10ms (see below npi_poll_cond)
#ifndef SRDY_INTERRUPT
		ret =  HAL_RNP_SRDY_CLR();
		if(TRUE == ret)
#else
		//Interruption case, In case of a SREQ, SRDY will go low a end generate an event.
		// the npiPollLock will prevent us to arrive to this test,
		// BUT an AREQ can immediately follow  a SREQ: SRDY will stay low for the whole process
		// In this case, we need to check that the SRDY line is still LOW or is HIGH.
		if(1)
#endif
		{
			debug_printf("Polling received...\n");

			//RNP is polling, retrieve the data
			*readbuf = 0; //Poll Command has zero data bytes.
			*(readbuf+1) = RPC_CMD_POLL;
			*(readbuf+2) = 0;
			ret = npi_spi_pollData((npiMsgData_t *)readbuf);
			debug_printf("Poll unLock SRDY ...\n");
			if (ret == NPI_LNX_SUCCESS)
			{
				//Check if polling was successful
				if ((readbuf[RPC_POS_CMD0] & RPC_CMD_TYPE_MASK) == RPC_CMD_AREQ)
				{
					((uint8 *)readbuf)[RPC_POS_CMD0] &=  RPC_SUBSYSTEM_MASK;
					ret = NPI_AsynchMsgCback((npiMsgData_t *)(readbuf));
					if (ret != NPI_LNX_SUCCESS)
					{
						// Exit thread to invoke report to main thread
						npi_poll_terminate = 1;
					}
				}
				else if (ret == NPI_LNX_ERROR_SPI_POLL_DATA_SRDY_CLR_TIMEOUT_POSSIBLE_RESET)
				{
					printf("[WARNING] Unexpected handshake received. RNP may have reset. \n");
				}
			}
			else
			{
				// Exit thread to invoke report to main thread
				npi_poll_terminate = 1;
			}

			if (!PollLockVar)
				ret = PollLockVarError();
			else
				PollLockVar = 0;

			if ( 0 == pthread_mutex_unlock(&npiPollLock))
			{
				pollStatus = TRUE;
				debug_printf("Poll unLock SRDY ...\n");
			}
			else
			{
				debug_printf("Poll unLock SRDY FAILED...\n");
			    npi_ipc_errno = NPI_LNX_ERROR_SPI_POLL_THREAD_POLL_UNLOCK;
			    ret = NPI_LNX_FAILURE;
				npi_poll_terminate = 1;
			}
		}
		else
		{
			if (!PollLockVar)
				ret = PollLockVarError();
			else
				PollLockVar = 0;

			if ( 0 == pthread_mutex_unlock(&npiPollLock))
			{
				debug_printf("Poll unLock SRDY ...\n");
			}
			else
			{
				debug_printf("Poll unLock SRDY FAILED...\n");
			    npi_ipc_errno = NPI_LNX_ERROR_SPI_POLL_THREAD_POLL_UNLOCK;
			    ret = NPI_LNX_FAILURE;
				npi_poll_terminate = 1;
			}
			pollStatus = FALSE;
		}


#ifdef SRDY_INTERRUPT
		debug_printf("POLL: Lock SRDY mutex (SRDY=%d) \n", global_srdy);
		pthread_cond_wait(&npi_srdy_H2L_poll, &npiPollLock);
		debug_printf("POLL: Locked SRDY mutex (SRDY=%d) \n", global_srdy);
#else
		if (!pollStatus) //If previous poll failed, wait 10ms to do another one, else do it right away to empty the RNP queue.
		{
			struct timespec expirytime;
			struct timeval curtime;

			gettimeofday(&curtime, NULL);
			expirytime.tv_sec = curtime.tv_sec;
			expirytime.tv_nsec = (curtime.tv_usec * 1000) + 10000000;
			if (expirytime.tv_nsec >= 1000000000) {
				expirytime.tv_nsec -= 1000000000;
				expirytime.tv_sec++;
			}
			pthread_cond_timedwait(&npi_poll_cond, &npi_poll_mutex, &expirytime);
		}
#endif
	}
	printf("POLL: Thread Exiting... \n");
	pthread_mutex_unlock(&npi_poll_mutex);

	char *errorMsg;
	if ( (ret != NPI_LNX_SUCCESS) && (npi_ipc_errno != NPI_LNX_ERROR_SPI_POLL_THREAD_SREQ_CONFLICT) )
	{
		errorMsg = "SPI Poll thread exited with error. Please check global error message\n";
	}
	else
	{
		errorMsg = "SPI Poll thread exited without error\n";
	}

	NPI_LNX_IPC_NotifyError(NPI_LNX_ERROR_MODULE_MASK(NPI_LNX_ERROR_SPI_POLL_THREAD_POLL_LOCK), errorMsg);

	return NULL;
}

/**************************************************************************************************
 * @fn          npi_termpoll
 *
 * @brief       Poll Thread terminate function
 *
 * input parameters
 *
 * @param      ptr
 *
 * output parameters
 *
 * None.
 *
 * @return      None.
 **************************************************************************************************
 */
static void npi_termpoll(void)
{
  //This will cause the Thread to exit
  npi_poll_terminate = 1;

#ifdef SRDY_INTERRUPT
	pthread_cond_signal(&npi_srdy_H2L_poll);
#else

	// In case of polling mechanism, send the Signal to continue
	pthread_cond_signal(&npi_poll_cond);
#endif

#ifdef SRDY_INTERRUPT
	pthread_mutex_destroy(&npiSrdyLock);
#endif
  pthread_mutex_destroy(&npi_poll_mutex);

  // wait till the thread terminates
  pthread_join(npiPollThread, NULL);

#ifdef SRDY_INTERRUPT
  pthread_join(npiEventThread, NULL);
#endif //SRDY_INTERRUPT
}

#ifdef SRDY_INTERRUPT
/**************************************************************************************************
 * @fn          npi_event_entry
 *
 * @brief       Poll Thread entry function
 *
 * input parameters
 *
 * @param      ptr
 *
 * output parameters
 *
 * None.
 *
 * @return      None.
 **************************************************************************************************
 */
static void *npi_event_entry(void *ptr)
{
	int result = -1;
	int ret = NPI_LNX_SUCCESS;
	int timeout = 2000; /* Timeout in msec. */
	struct pollfd pollfds[1];
	int val;

	printf("EVENT: Thread Started \n");

	/* thread loop */
	while (!npi_poll_terminate) {
		{
			memset((void*) pollfds, 0, sizeof(pollfds));
			pollfds[0].fd = GpioSrdyFd; /* Wait for input */
			pollfds[0].events = POLLPRI; /* Wait for input */
			result = poll(pollfds, 1, timeout);
//			debug_printf("poll() timeout\n");
			switch (result) {
			case 0:
			{
				//Should not happen by default no Timeout.
				result = 2; //FORCE WRONG RESULT TO AVOID DEADLOCK CAUSE BY TIMEOUT
				debug_printf("[INT]:poll() timeout\n");
#ifdef __BIG_DEBUG__
				if (  NPI_LNX_FAILURE == (val = HalGpioSrdyCheck(1)))
				{
					ret = val;
					npi_poll_terminate = 1;
				}
#endif
				debug_printf("[INT]: SRDY: %d\n", val);
				break;
			}
			case -1:
			{
				debug_printf("[INT]:poll() error \n");
				npi_ipc_errno = NPI_LNX_ERROR_SPI_EVENT_THREAD_FAILED_POLL;
			    ret = NPI_LNX_FAILURE;
				// Exit clean so main knows...
				npi_poll_terminate = 1;
			}
			default:
			{
				char * buf[64];
				read(pollfds[0].fd, buf, 64);
				result = global_srdy = HalGpioSrdyCheck(1);
				debug_printf("[INT]:Set global SRDY: %d\n", global_srdy);

			}
			break;
			}
		}
		fflush(stdout);

		if (FALSE == result) //Means SRDY switch to low state
		{
			if ( (NPI_LNX_FAILURE == (ret = HalGpioMrdyCheck(1))))
			{
				debug_printf("[INT]:Fail to check MRDY \n");
			    ret = NPI_LNX_FAILURE;
				// Exit clean so main knows...
				npi_poll_terminate = 1;
			}

			if (ret != NPI_LNX_FAILURE)
			{
				//MRDY High, This is a request from the RNP
				debug_printf("[INT]: MRDY High??: %d \n", ret);
				debug_printf("[INT]: send H2L to poll (srdy = %d)\n",
						global_srdy);
				pthread_cond_signal(&npi_srdy_H2L_poll);
			}

		} 
		else 
		{
			//Unknow Event
			//Do nothing for now ...
			//debug_printf("Unknow Event or timeout, ignore it, result:%d \n",result);
		}

	}

	return NULL;
}
#endif
/**************************************************************************************************
*/
