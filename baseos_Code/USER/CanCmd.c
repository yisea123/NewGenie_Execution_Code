#include    <ucos_ii.h>

#include    <cpu.h>
#include    <lib_def.h>
#include    <lib_mem.h>
#include    <lib_str.h>

#include    <string.h>
#include   <stdarg.h>

#include "stm32f4xx.h"

#include "CanCmd.h"

#include "memory.h"
#include "msg.h"
#include "timer.h"

#include "stm32_eval.h"

#include "Can_driver.h"

#include "app_cfg.h"

#include "Config.h"

#include "Beep.h"

#include "Errorcode.h"

#include "common.h"

#include "sapp.h"

#include "cminterface.h"

#include "osal_snv.h"

#include "RTC_Driver.h"

#include "Display.h"

#include "CanCmd.h"

#include "Relay.h"

#include "Dica.h"

#include "Task.h"

#include "zb.h"

#define CAN_500kbps   (SystemCoreClock/4/8/500000)       
#define CAN_250kbps   (SystemCoreClock/4/8/250000)
#define CAN_125kbps   (SystemCoreClock/4/8/125000)   
#define CAN_62_5kbps  (SystemCoreClock/4/8/62500)   
#define CAN_31_25kbps (SystemCoreClock/4/8/31250)      

CAN_CCB  CanCcb[MAX_CAN_CCB_NUMBER];

CAN_Rcv_buff  CanRcvBuff[CAN_MAX_RCV_BUFFER];

CAN_Snd_buff  CanSndBuff[MAX_CAN_OUTPUT_BUFFER];

UINT8  ucSndbufAllocMask = 0;

UINT8  ucSndBufFront = 0;

UINT8  ucSndBufRear = 0;

UINT8  aSndBufQueue[MAX_CAN_OUTPUT_BUFFER];

UINT8  ucCanOutFlag = FALSE;

UINT8  ucCanBusySecond = 0;

UINT16 CanAddress = 0;

UINT16 CanHashAddr = 0;

UINT8 ucZombieCcbIndex;

CAN_Snd_Empty_CallBack gCanSndEmptyCB;

#define APP_PROTOL_CHECK_HEART_BEAT(usDuration) ((usDuration != 0XFFFF) && (usDuration != 0))
#define APP_PROTOL_HEART_BEAT_CLIENT_PERIOD 20000
#define APP_PROTOL_HEART_BEAT_HOST_PERIOD   (APP_PROTOL_HEART_BEAT_CLIENT_PERIOD - 5000)

typedef struct
{
    MsgHead msgHead;
    void *para;
    void *para1;
}CANCMD_MSG;


#define CANCMD_MSG_LENGHT (sizeof(CANCMD_MSG)-sizeof(MsgHead))

typedef void (*cancmd_msg_cb)(void * para);

#define CAN_CMD_RSP_DELAY(index,ms) (index*ms)

void CanCcbHeartBeatTimer(void *para);
uint8 CanCcbAfDataHostSndHeartBeatMsg(void);

UINT8 CanAllocSndBuff(void)
{
    UINT8 ucLoop ;

    for (ucLoop= 0; ucLoop < MAX_CAN_OUTPUT_BUFFER; ucLoop++)
    {
        if (ucSndbufAllocMask & (1 << ucLoop))
        {
            continue;
        }
        ucSndbufAllocMask |= (1 << ucLoop); 
        
        return ucLoop;
    }

    return MAX_CAN_OUTPUT_BUFFER; 
}

void CanFreeSndBuff(UINT8 ucIdx)
{
    if (ucIdx < MAX_CAN_OUTPUT_BUFFER)
    {
        ucSndbufAllocMask &= ~(1 << ucIdx); 
    }
}

UINT8 CanSndBufPop(void)
{
    UINT8 ucIdx ;

    if (ucSndBufRear == ucSndBufFront)
    {
        // queue full
        return MAX_CAN_OUTPUT_BUFFER;
    }
    
    ucIdx = aSndBufQueue[ucSndBufRear] ;
    
    ucSndBufRear = (ucSndBufRear +1)% MAX_CAN_OUTPUT_BUFFER ;

    return ucIdx;

}

UINT8 CanSndBufFull(void)
{
    return (((ucSndBufFront + 1) % MAX_CAN_OUTPUT_BUFFER )== ucSndBufRear);
}

UINT8 CanSndBufEmpty(void)
{
    return (ucSndBufFront == ucSndBufRear);
}

void CanResetTimer(void *para)
{
    HAL_SYSTEM_RESET();
}


/*********************************************************************
 * Function:        void CanCcbMsgSndCb(void)
 *
 * PreCondition:    None
 *
 * Input:           None
 *
 * Output:          None
 *
 * Side Effects:    None
 *
 * Overview:        Callback handler for sent CAN message 
 *
 * Note:            None.
 ********************************************************************/
void CanMsgSndCb(UINT8 ucIndex, UINT8 aucCmd[])
{
    if ((sappFlags & (1 << SAPP_CMD_RESET))
        || (sappFlags & (1 << SAPP_CMD_SYS_INIT)))
    {
        system_timeout(500,SYS_TIMER_ONE_SHOT,100,CanResetTimer,NULL,&CanCcb[ucIndex].to4Callback);
    }
}


/*********************************************************************
 * Function:        void SndCanData(void)
 *
 * PreCondition:    None
 *
 * Input:           None
 *
 * Output:          None
 *
 * Side Effects:    None
 *
 * Overview:        Send Can data
 *
 * Note:            None.
 ********************************************************************/
void SndCanData(void)
{
    UINT8 MsgLen;
    UINT8 ucTmp;
#if OS_CRITICAL_METHOD == 3         /* Allocate storage for CPU status register */
    OS_CPU_SR     cpu_sr = 0;
#endif      
    OS_ENTER_CRITICAL();

    if (!CanSndBufEmpty()) // there are something to be send
    {
        CAN_FRAME frame;
    
        ucTmp = aSndBufQueue[ucSndBufRear];

        
        if (CanSndBuff[ucTmp].data_len >= 8)
        {
            MsgLen = 8;
        }
        else
        {
            MsgLen = CanSndBuff[ucTmp].data_len;
        }        

        frame.can_dlc = MsgLen;

        CAN_BUILD_ADDRESS_IDENTIFIER(frame.can_id,CanSndBuff[ucTmp].usSrcCanAddress,CanSndBuff[ucTmp].usDstCanAddress);
        
        memcpy(frame.data,CanSndBuff[ucTmp].dat,MsgLen);
    
        if (STM32_CANSendMsgNoWait(&frame))
        {
            //MainBeepWithDuration(1);
        
            CanSndBuff[ucTmp].data_len -= MsgLen;

            if (0 == CanSndBuff[ucTmp].data_len)
            {
                // free buffer
                FreeMem(CanSndBuff[ucTmp].head);
            
                CanSndBufPop();  
        
                CanFreeSndBuff(ucTmp);

                CanMsgSndCb(CanSndBuff[ucTmp].ucCcbIndex,CanSndBuff[ucTmp].aucCmd);
            }
            else
            {
                CanSndBuff[ucTmp].dat += MsgLen;
            }
            ucCanOutFlag = TRUE;
        }
    }
    else
    {
        ucCanOutFlag = FALSE;
        ucCanBusySecond = 0;
        
        CAN_ITConfig(CAN1, CAN_IT_TME, DISABLE);
        
        if (gCanSndEmptyCB)(*gCanSndEmptyCB)();

    }
    OS_EXIT_CRITICAL();

}

void CanSndBufPush(UINT8 ucIdx)
{
    if (((ucSndBufFront + 1) % MAX_CAN_OUTPUT_BUFFER )== ucSndBufRear)
    {
        // queue full
        return ;
    }
    aSndBufQueue[ucSndBufFront] = ucIdx;
    
    ucSndBufFront = (ucSndBufFront +1)% MAX_CAN_OUTPUT_BUFFER ;

    // 
    if (!ucCanOutFlag)
    {
        SndCanData();
    }

}


void CanCheckZombieCcb(void)
{

    CAN_CCB *pCcb = NULL;
    
    ucZombieCcbIndex = (ucZombieCcbIndex+1)%MAX_CAN_CCB_NUMBER;

    pCcb = &CanCcb[ucZombieCcbIndex];
    
    pCcb = pCcb;

    // current do nothing
}


void CanCcbInit(void)
{
    UINT8 ucLoop;
    for (ucLoop = 0; ucLoop < MAX_CAN_CCB_NUMBER; ucLoop++)
    {
        memset(&CanCcb[ucLoop],0,sizeof(CAN_CCB));
        CanCcb[ucLoop].ucCcbIdx = ucLoop;
    }

}

void CanCcbInitCanId(void)
{
    UINT8 ucLoop;
    for (ucLoop = 0; ucLoop < MAX_CAN_CCB_NUMBER; ucLoop++)
    {
        switch(ucLoop)
        {
        default:
            CanCcb[ucLoop].usLocalCanAddress  = CanAddress;
            break;
        case CAN_LOCAL_FM_INDEX:
            CanCcb[ucLoop].usLocalCanAddress  = CanAddress + 1;
            break;
        }
    }

}


void CanTranceiveBufInit(void)
{
    UINT8 ucLoop;
    for (ucLoop = 0; ucLoop < CAN_MAX_RCV_BUFFER; ucLoop++)
    {
        CanRcvBuff[ucLoop].len = 0;
        CanRcvBuff[ucLoop].data_len = 0;
        CanRcvBuff[ucLoop].head = 0;
        CanRcvBuff[ucLoop].dat = 0;
    }

    ucSndbufAllocMask = 0;

    ucSndBufFront = 0;

    ucSndBufRear = 0;

    ucCanOutFlag = 0;

    ucCanBusySecond = 0;
    
}



void CanCmd_report(void *para,void *para1)
{
   Message *Msg;
   Msg = MessageAlloc(PID_SELF,CANCMD_MSG_LENGHT);

   if (Msg)
   {
       CANCMD_MSG *dmsg = (CANCMD_MSG *)Msg;
       dmsg->msgHead.nMsgType = SELF_MSG_CODE_USER_CANCMD;
       dmsg->para = para;
       dmsg->para1 = para1;
       MessageSend(Msg);
   }
}

void CanCmdInitizeCAN(void)
{
    CAN_InitTypeDef        CAN_InitStructure;
    CAN_FilterInitTypeDef  CAN_FilterInitStructure;

    UINT32 dwCanId;
    UINT32 dwMask;
    
        /* CAN register init */
    CAN_DeInit(CAN1);
    CAN_StructInit(&CAN_InitStructure);
    
    /* CAN cell init */  //  ylf: BaudRate = PCLK2/(CAN_Prescaler)/(BS1+BS2+1)
    CAN_InitStructure.CAN_TTCM = DISABLE;
    //CAN_InitStructure.CAN_ABOM = DISABLE;
    CAN_InitStructure.CAN_ABOM = ENABLE;// 20160309 modifed for DISABLE->ENABLE, enable auto error recover
    CAN_InitStructure.CAN_AWUM = DISABLE;
    //CAN_InitStructure.CAN_NART = ENABLE; // disable retransmition
    CAN_InitStructure.CAN_NART = DISABLE; // enable retransmition
    CAN_InitStructure.CAN_RFLM = DISABLE;
    CAN_InitStructure.CAN_TXFP = ENABLE;   // ylf: The transmit mailboxes as a transmit FIFO
    CAN_InitStructure.CAN_Mode = CAN_Mode_Normal;// CAN_Mode_Normal; // CAN_Mode_LoopBack;
    //CAN_InitStructure.CAN_SJW  = CAN_SJW_1tq;
    //CAN_InitStructure.CAN_BS1  = CAN_BS1_6tq;  // ylf: PROP_SEG and PHASE_SEG1
    //CAN_InitStructure.CAN_BS2  = CAN_BS2_7tq;  // ylf: PHASE_SEG2
    //CAN_InitStructure.CAN_Prescaler = 24;//CAN_125kbps;

    CAN_InitStructure.CAN_SJW  = CAN_SJW_1tq;
    CAN_InitStructure.CAN_BS1  = CAN_BS1_4tq; // ylf: PROP_SEG and PHASE_SEG1
    CAN_InitStructure.CAN_BS2  = CAN_BS2_3tq;  // ylf: PHASE_SEG2
    CAN_InitStructure.CAN_Prescaler = CAN_125kbps;
    
    
    STM_EVAL_CANInit(&CAN_InitStructure);
    

    /* CAN filter init */
#ifdef CAN_ADR_FILTER    
    /* CAN filter init */

    dwMask = 0x1FFF;
    dwCanId = (((CanAddress & ~1) & 0x3ff) << 3)|CAN_ID_EXT|CAN_RTR_DATA;
    CAN_FilterInitStructure.CAN_FilterNumber = 0;
    CAN_FilterInitStructure.CAN_FilterMode       = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale      = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 0;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);

    CAN_FilterInitStructure.CAN_FilterNumber = 1;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 1;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure); 

    dwMask = 0x1FFF;
    dwCanId = ((((CanAddress & ~1) + 1) & 0x3ff) << 3)|CAN_ID_EXT|CAN_RTR_DATA;
    CAN_FilterInitStructure.CAN_FilterNumber = 2;
    CAN_FilterInitStructure.CAN_FilterMode       = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale      = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 0;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);

    CAN_FilterInitStructure.CAN_FilterNumber = 3;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 1;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);     


    dwCanId = ((0x3ffUL) << 3)|CAN_ID_EXT|CAN_RTR_DATA;
    CAN_FilterInitStructure.CAN_FilterNumber = 4;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 0;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);
    
    CAN_FilterInitStructure.CAN_FilterNumber = 5;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = (dwMask >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = dwMask  & 0XFFFF; // least 3bits as net mask
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 1;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure); 
#else
    /* CAN filter init */
    dwCanId = 0;
    CAN_FilterInitStructure.CAN_FilterNumber = 0;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = 0x0000;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = 0x0000;
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 0;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);
    
    CAN_FilterInitStructure.CAN_FilterNumber = 1;
    CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_32bit;
    CAN_FilterInitStructure.CAN_FilterIdHigh = (dwCanId >> 16) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterIdLow =  (dwCanId) & 0XFFFF;
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = 0x0000;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow = 0x0000;
    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = 1;
    CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure); 

#endif

    CAN_ITConfig(CAN1, CAN_IT_FMP0 /*| CAN_IT_FF0 | CAN_IT_FOV0*/, ENABLE);  // fifo0�ж�
    CAN_ITConfig(CAN1, CAN_IT_FMP1 /*| CAN_IT_FF1 | CAN_IT_FOV1*/, ENABLE);   // fifo1
    CAN_ITConfig(CAN1, CAN_IT_TME, DISABLE);                              // �����ж�
    //CAN_ITConfig(CAN1, CAN_IT_EWG | CAN_IT_EPV | CAN_IT_BOF | CAN_IT_LEC 
    //            | CAN_IT_ERR | CAN_IT_WKU | CAN_IT_SLK, ENABLE);         // ERR�ж�

}


void CanCmdInit(void)
{

  // 1, retrive configurations
  if ( osal_snv_read( NV_CANID_ID, sizeof ( CanAddress ), &CanAddress ) != ERROR_SUCCESS )
  {
      CanAddress = 0x3FF;
  }

  if (CanAddress != APP_DEV_TYPE_EXE_BOARD)
  {
      CanAddress = APP_DEV_TYPE_EXE_BOARD;
      
      Config_SetItem(NV_CANID_ID,sizeof(CanAddress),&CanAddress);
  }

  ucZombieCcbIndex = 0;

  CanCcbInit();

  CanCcbInitCanId();
  
  CanTranceiveBufInit();

  // 3. init hardware
  CanCmdInitizeCAN();

  gCanSndEmptyCB = NULL;
 
}


void CanCleanSndBuf()
{
    UINT8 ucIdx;

#if OS_CRITICAL_METHOD == 3         /* Allocate storage for CPU status register */
     OS_CPU_SR     cpu_sr = 0;
#endif      
    OS_ENTER_CRITICAL();

    do 
    {
        ucIdx = CanSndBufPop();
        if (ucIdx < MAX_CAN_OUTPUT_BUFFER)
        {
            // free buffer
            FreeMem(CanSndBuff[ucIdx].head);
            
            CanFreeSndBuff(ucIdx);
            
        }
    }while(ucIdx < MAX_CAN_OUTPUT_BUFFER);

    ucSndbufAllocMask = 0;
    ucSndBufRear = 0;
    ucSndBufFront = 0;
    ucCanOutFlag = FALSE;
    ucCanBusySecond = 0;
    
    OS_EXIT_CRITICAL();
    
}

void CanBusyCheck(void)
{
    if (ucSndbufAllocMask
        || ucCanOutFlag)
    {
       ucCanBusySecond++;
       if (ucCanBusySecond >= CAN_MAX_BUSY_SECOND)
       {
           // must be error, just reset CAN
           CanCleanSndBuf();
           
           CanCmdInitizeCAN();
       }
    }

    CanCheckZombieCcb();
}


/*********************************************************************
 * Function:        void RcvCanData(UINT8 ,UINT8 )
 *
 * PreCondition:    None
 *
 * Input:           Address Index, Can Link Packet Length
 *
 * Output:          None
 *
 * Side Effects:    None
 *
 * Overview:         CAN Network layer message reassemble
 *
 * Note:            None.
 ********************************************************************/
void RcvCanData(UINT8 ucIndex,CanRxMsg *pCanMsg )
{
   UINT16 usCanLen;
   
   if (0 == CanRcvBuff[ucIndex].len)
   {
       // judge message tag
       if (pCanMsg->Data[0] != RPC_UART_SOF)
       {
          // invalid tag
          return ;
       }

       // the head of networklayer have been received, this must be the second segment of networklayer
       usCanLen = pCanMsg->Data[1 + RPC_POS_LEN] + RPC_UART_FRAME_OVHD + RPC_FRAME_HDR_SZ;

       CanRcvBuff[ucIndex].len = usCanLen;

       CanRcvBuff[ucIndex].head = AllocMem(usCanLen);
       
       CanRcvBuff[ucIndex].dat = CanRcvBuff[ucIndex].head;

       if (CanRcvBuff[ucIndex].head)
       {
           memcpy(CanRcvBuff[ucIndex].dat,pCanMsg->Data,pCanMsg->DLC);
       }
       CanRcvBuff[ucIndex].dat += pCanMsg->DLC;

       CanRcvBuff[ucIndex].data_len = pCanMsg->DLC;

       CanRcvBuff[ucIndex].ucSubCanAdr = ucIndex;

       CanRcvBuff[ucIndex].usSrcAddr  = CAN_SRC_ADDRESS(pCanMsg->ExtId);
   }
   else
   {
       // other segment
       usCanLen = CanRcvBuff[ucIndex].data_len + pCanMsg->DLC;

       if (usCanLen > CanRcvBuff[ucIndex].len
          || ((usCanLen < CanRcvBuff[ucIndex].len)
              && (pCanMsg->DLC < 8)))
       {
           // invalid message
           CanRcvBuff[ucIndex].len = 0;

           // free buffer
           FreeMem(CanRcvBuff[ucIndex].head);

           CanRcvBuff[ucIndex].head = NULL;
           
           return;
       }
       if (CanRcvBuff[ucIndex].head)
       {
           memcpy(CanRcvBuff[ucIndex].dat,pCanMsg->Data,pCanMsg->DLC);
       } 
       CanRcvBuff[ucIndex].dat += pCanMsg->DLC;
       CanRcvBuff[ucIndex].data_len += pCanMsg->DLC;
       
   }


   if (CanRcvBuff[ucIndex].data_len == CanRcvBuff[ucIndex].len
    && 0 != CanRcvBuff[ucIndex].len)
   {
      // a frame is received

      if (CanRcvBuff[ucIndex].head)
      {
          switch(ucIndex)
          {
          case CAN_BROADCAST_INDEX:
          case CAN_LOCAL_EXE_INDEX:
              CanRcvFrame(CAN_LOCAL_EXE_INDEX,ucIndex);
              break;
          case CAN_LOCAL_FM_INDEX:
              CanRcvFrame(CAN_LOCAL_FM_INDEX,ucIndex);
              break;
          }
      }
      // clear rcv buffer
      CanRcvBuff[ucIndex].len = 0;
      CanRcvBuff[ucIndex].data_len = 0;
      if (CanRcvBuff[ucIndex].head)
      {
           FreeMem(CanRcvBuff[ucIndex].head);
      }
      CanRcvBuff[ucIndex].head = NULL;
      
   }
}

UINT8 CanCmdSetAddr(uint8 *dat)
{
    // compare elecid
    int idx = 0;
    
    uint8 *data = &dat[RPC_POS_DAT0];
    
    if (data[idx] != (HAL_ELEC_ID_SIZE << 1))
    {
        return FALSE; // invalid message
    }
    
    idx += 1;
    
    if (0 != CmpDeviceElecId(&data[idx]))
    {
        return FALSE; // not target to us
    }
    
    {
        idx += (HAL_ELEC_ID_SIZE << 1);
    
        CanAddress = (data[idx] << 8)|data[idx+1];//BUILD_UINT16(data[idx+1],data[idx]) ;
    
        CanAddress &= 0x7ff;

        CanCmdInitizeCAN();

        CanCcbInitCanId();
    }
    return TRUE;

}

void CanCcbSetOnLineFlag(uint8_t ucCcbIdx,uint8_t bFlag)
{
    CanCcb[ucCcbIdx].bit1OnlineState = bFlag;
}

/* Callback */

UINT8 PidCanProcess(Message *pMsg)
{
    UINT8 ucTmp;

    UINT32 Identifier;
    
    CanRxMsg *pCanMsg = (CanRxMsg *)pMsg->data;

    Identifier = (UINT32)pCanMsg->ExtId;

    Identifier = CAN_DST_ADDRESS(Identifier);

    if (CAN_BROAD_CAST_ADDRESS == Identifier)
    {
        ucTmp = CAN_BROADCAST_INDEX ;
    }
    else if (APP_DEV_TYPE_EXE_BOARD == CAN_ADDRESS(Identifier))
    {
        ucTmp = CAN_LOCAL_EXE_INDEX;
    }  
    else /*if (APP_DEV_TYPE_FLOW_METER == CAN_ADDRESS(Identifier))*/
    {
        ucTmp = CAN_LOCAL_FM_INDEX;
    }  

    RcvCanData(ucTmp,pCanMsg);

    return 0;
}



/**************************************************************************************************
 * @fn          SHZNAPP_CanResp
 *
 * @brief       Make the SB response.
 *
 * input parameters
 *
 * None.
 *
 * output parameters
 *
 * None.
 *
 * @return      TRUE if the downloaded code has been enabled; FALSE otherwise.
 */
uint8 SHZNAPP_CanResp(uint16 usDstCanAdr,uint16 usSrcCanAdr)
{
  uint8 fcs = 0, len = sbTxBuf[RPC_POS_LEN] + RPC_FRAME_HDR_SZ;
  uint8 rtrn = FALSE;
  uint8 idx;

  if (INVALID_CAN_ADDRESS(usSrcCanAdr))
  {
      return rtrn;
  }
  
  for ( idx = RPC_POS_LEN; idx < len; idx++)
  {
    fcs ^= sbTxBuf[idx];
  }
  sbTxBuf[len] = fcs;


  sbTxBuf[-1] = RPC_UART_SOF;

  {
    UINT8 ucIdx ;

    UINT8 *pData;

    pData = AllocMem(len + RPC_UART_FRAME_OVHD); // 1byte for SOF ,1byte for checksum

    if (!pData)
    {
        return rtrn;
    }

    // send ack to network controller
    ucIdx = CanAllocSndBuff();

    if(ucIdx >= MAX_CAN_OUTPUT_BUFFER)
    {
        // no buffer
        FreeMem(pData);
        return rtrn;
    }
    
    CanSndBuff[ucIdx].head         = pData;
    CanSndBuff[ucIdx].dat          = pData; // init to pointer to begin of data area
    CanSndBuff[ucIdx].ucCcbIndex   = 0;
    CanSndBuff[ucIdx].len          = len + RPC_UART_FRAME_OVHD; 
    CanSndBuff[ucIdx].data_len     = CanSndBuff[ucIdx].len;            
    CanSndBuff[ucIdx].ucCanAdrType = 0;
    CanSndBuff[ucIdx].usSrcCanAddress = usSrcCanAdr;
    CanSndBuff[ucIdx].usDstCanAddress = usDstCanAdr;

    CanSndBuff[ucIdx].aucCmd[0] = sbTxBuf[RPC_POS_CMD0];
    CanSndBuff[ucIdx].aucCmd[1] = sbTxBuf[RPC_POS_CMD1];

    memcpy(pData,sbTxBuf - 1,len + RPC_UART_FRAME_OVHD);
    
    // push to queue
    CanSndBufPush(ucIdx);
  }
  
  return TRUE;
}


void CanCcbHeartBeatTimer_msg_handler(void *para)
{
    CAN_CCB *pCcb = (CAN_CCB *)para;
    if (pCcb->bit3HeartBeat)
    {
        pCcb->bit3HeartBeat--;

        if (pCcb->bit3HeartBeat)
        {
            system_timeout(pCcb->usHeartBeatPeriod,SYS_TIMER_ONE_SHOT,pCcb->usHeartBeatPeriod,CanCcbHeartBeatTimer,pCcb,&pCcb->to4HB);
            
            return ;
        }
    }

    pCcb->bit1Registered = FALSE;

    // lost heartbeat
    CanCcbSetOnLineFlag(pCcb->ucCcbIdx,FALSE);

    Disp_CanItfNotify(pCcb->ucCcbIdx,DISP_CANITF_CODE_LOST_HB);
    
    VOS_LOG(VOS_LOG_DEBUG,"lost heartbeat\r\n");

}


void CanCcbHeartBeatTimer(void *para)
{
    CanCmd_report(CanCcbHeartBeatTimer_msg_handler,para);

}

void CanCcbTimeout(UINT8 ucIndex,UINT16 usPara)
{
    CanCcb[ucIndex].ucMachineState = MACHINE_STATE_IDLE;
}


void CanCcbTimerProc(uint8_t ucTimeId,uint16_t AddData)
{
    if (ucTimeId >= TIMER_CCB_BEGIN && ucTimeId < TIMER_CCB_END)
    {
        CanCcbTimeout(ucTimeId - TIMER_CCB_BEGIN,AddData);
    }

}


UINT16 CanCmdHashAdr(void)
{
    // calc hash address
    UINT16 usCanHashAddr;
    usCanHashAddr = (HashDeviceElecId())|(SysTick->VAL & 0xffff);
    
    srand(usCanHashAddr);
    
    usCanHashAddr = APP_PROTOL_CANID_DYNAMIC_BEGIN + (rand() % APP_PROTOL_CANID_DYNAMIC_RANGE);

    return usCanHashAddr;
}



void CanCcbAfDataClientMsgCnfCommProc(UINT8 ucCcbIndex)
{

    CanCcb[ucCcbIndex].bit3HeartBeat = APP_EXE_HB_NUM;

    CanCcbSetOnLineFlag(ucCcbIndex,TRUE);

}

uint8 CanCcbAfDataClientOnLineNotiCnfMsg(UINT8 ucCcbIndex,uint8_t *msg)
{

    APP_PACKET_ONLINE_NOTI_CONF_STRU *pmg = (APP_PACKET_ONLINE_NOTI_CONF_STRU *)msg; 

    //ucCcbIndex = CAN_LOCAL_EXE_INDEX;

    CanCcb[ucCcbIndex].ucMachineState    = MACHINE_STATE_IDLE;

    CanCcb[ucCcbIndex].bit1Registered    = TRUE;

    RmvTimer(TIMER_CCB_BEGIN + ucCcbIndex);

    switch(ucCcbIndex)
    {
    default:
        {
            APP_PACKET_ONLINE_NOTI_CONF_EXE_STRU *pConf = (APP_PACKET_ONLINE_NOTI_CONF_EXE_STRU *)pmg->aucInfo;
            
            CanCcb[ucCcbIndex].usHeartBeatPeriod = pConf->usHeartBeatPeriod;

            if (APP_PROTOL_CHECK_HEART_BEAT(CanCcb[ucCcbIndex].usHeartBeatPeriod))
            {
                //start time
                system_timeout(CanCcb[ucCcbIndex].usHeartBeatPeriod,SYS_TIMER_ONE_SHOT,CanCcb[ucCcbIndex].usHeartBeatPeriod,CanCcbHeartBeatTimer,&CanCcb[ucCcbIndex],&CanCcb[ucCcbIndex].to4HB);
            }
            else
            {
                system_untimeout(&CanCcb[ucCcbIndex].to4HB);
            }   
            
        }
        break;
    case CAN_LOCAL_FM_INDEX:
        {
            APP_PACKET_ONLINE_NOTI_CONF_FM_STRU *pConf = (APP_PACKET_ONLINE_NOTI_CONF_FM_STRU *)pmg->aucInfo;
            
            CanCcb[ucCcbIndex].usHeartBeatPeriod = pConf->usHeartBeatPeriod;
            
        }
        break;
    case CAN_ZIGBEE_INDEX:
        {
            APP_PACKET_ONLINE_NOTI_CONF_FM_STRU *pConf = (APP_PACKET_ONLINE_NOTI_CONF_FM_STRU *)pmg->aucInfo;
            
            CanCcb[ucCcbIndex].usHeartBeatPeriod = pConf->usHeartBeatPeriod;
            
        }
        break;
    }


    CanCcbAfDataClientMsgCnfCommProc(ucCcbIndex);

    return 0;
}

void CanCcbHeartBeat_msg_handler(void *para)
{
    UINT8 ucCcbIndex = (UINT8)(UINT32)para;
    UINT8 ucTransId  = (((UINT32)para) >> 8)& 0XFF;

    APP_PACKET_HEART_BEAT_RSP_STRU tsr;

    tsr.hdr.ucLen       = APP_POROTOL_PACKET_HEART_BEAT_RSP_PAYLOAD_LENGTH;
    tsr.hdr.ucMsgType   = APP_PACKET_COMM_HEART_BEAT|APP_PROTOL_PACKET_RSP_MASK;
    tsr.hdr.ucTransId   = ucTransId;
    tsr.hdr.ucDevType   = APP_DEV_TYPE_EXE_BOARD;
 
    CanSndSappCmd(ucCcbIndex,SAPP_CMD_DATA,(uint8_t *)&tsr,APP_POROTOL_PACKET_HEART_BEAT_RSP_TOTAL_LENGTH);
    
}

void CanCcbHeartBeat(void *para)
{
     CanCmd_report(CanCcbHeartBeat_msg_handler,para);
}

uint8_t CanCcbAfDataClientHeartBeatMsg(UINT8 ucCcbIndex,uint8_t *msg)
{

    APP_PACKET_HEART_BEAT_REQ_STRU *pmg = (APP_PACKET_HEART_BEAT_REQ_STRU *)msg; 

    //ucCcbIndex = CAN_LOCAL_EXE_INDEX;
    
    if (APP_PROTOL_CHECK_HEART_BEAT(CanCcb[ucCcbIndex].usHeartBeatPeriod))
    {
        system_timeout(CanCcb[ucCcbIndex].usHeartBeatPeriod,SYS_TIMER_ONE_SHOT,CanCcb[ucCcbIndex].usHeartBeatPeriod,CanCcbHeartBeatTimer,&CanCcb[ucCcbIndex],&CanCcb[ucCcbIndex].to4HB);
    }
#if 0
    // send response message
    {
       APP_PACKET_HEART_BEAT_RSP_STRU tsr;

       tsr.hdr.ucLen   = APP_POROTOL_PACKET_HEART_BEAT_RSP_PAYLOAD_LENGTH;
       tsr.hdr.ucType  = APP_PACKET_COMM_HEART_BEAT|APP_PROTOL_PACKET_RSP_MASK;

       CanSndSappCmd(ucCcbIndex,SAPP_CMD_DATA,(uint8_t *)&tsr,APP_POROTOL_PACKET_HEART_BEAT_RSP_TOTAL_LENGTH);
        
    }
    // MainBeepWithDuration(1);
#endif
    
    // VOS_LOG(VOS_LOG_DEBUG,"1 rcv heartbeat\r\n");

    system_timeout(CAN_CMD_RSP_DELAY(ucCcbIndex,20),SYS_TIMER_ONE_SHOT,CAN_CMD_RSP_DELAY(ucCcbIndex,20),CanCcbHeartBeat,(void *)(ucCcbIndex|(pmg->hdr.ucTransId << 8)),&CanCcb[ucCcbIndex].to4DelayExcu);

    CanCcbAfDataClientMsgCnfCommProc(ucCcbIndex);

    return 0;
}


uint8 CanCcbAfDataClientClientHostResetMsg(UINT8 ucCcbIndex,uint8_t *msg)
{
    //ucCcbIndex = CAN_LOCAL_EXE_INDEX;

    CanCcb[ucCcbIndex].bit1OnlineState = 0;

	CanCcb[ucCcbIndex].bit1Registered = 0;

    CanCcb[ucCcbIndex].ucMachineState = MACHINE_STATE_IDLE;

    /* sub module restore to init state */
    InputReset();
    
    Disp_Reset();

    return 0;
}


uint8 CanCcbAfDataRptAckMsg(UINT8 ucCcbIndex,uint8_t *msg)
{
    APP_PACKET_CLIENT_RPT_IND_STRU *pRpt = (APP_PACKET_CLIENT_RPT_IND_STRU *)msg;

    switch(pRpt->ucRptType)
    {
    case APP_PACKET_DIN_STATUS:
        {
            APP_PACKET_RPT_DIN_STRU *pDin = (APP_PACKET_RPT_DIN_STRU *)pRpt->aucData;

            Disp_DinReportAck(pDin->ucState);
        }
        break;
    default:
        break;
    }

    return 0;
}

uint8 CanCcbAfDataClientOpsMsg(UINT8 ucCcbIndex,uint8_t *msg)
{
    char buf[16];

    APP_PACKET_EXE_STRU *pReq = (APP_PACKET_EXE_STRU *)msg;
    APP_PACKET_EXE_STRU *pRsp = (APP_PACKET_EXE_STRU *)buf;

    APP_PACKET_EXE_TOC_RSP_STRU *pCont = (APP_PACKET_EXE_TOC_RSP_STRU *)pRsp->aucData;

    pRsp->hdr            = pReq->hdr;
    pRsp->hdr.ucMsgType |= 0x80;

    pRsp->ucOpsType = pReq->ucOpsType;

    pRsp->hdr.ucLen = 1 + sizeof(APP_PACKET_EXE_TOC_RSP_STRU);

    pCont->ucRslt  = 0; /* always successful */

    switch(pReq->ucOpsType)
    {
    case APP_PACKET_EXE_TOC:
        {
            /* Notify Display Itf to start TOC procedure */
            APP_PACKET_EXE_TOC_REQ_STRU *pToc = (APP_PACKET_EXE_TOC_REQ_STRU *)pReq->aucData;
            
            Disp_CanItfToc(pToc);
        }
        break;
    default:
        break;
    }

    CanSndSappCmd(ucCcbIndex,SAPP_CMD_DATA,(uint8_t *)pRsp,APP_PROTOL_HEADER_LEN + pRsp->hdr.ucLen);

    return 0;
}


uint8 CanCcbAfDataMsg(UINT8 ucCcbIndex)
{
    APP_PACKET_COMM_STRU *pmg = (APP_PACKET_COMM_STRU *)&sbRxBuf[RPC_POS_DAT0]; 
    switch((pmg->ucMsgType & 0x7f))
    {
    case APP_PAKCET_COMM_ONLINE_NOTI:
        if ((pmg->ucMsgType & 0x80))
        {
            return CanCcbAfDataClientOnLineNotiCnfMsg(ucCcbIndex,(uint8_t *)pmg);
        }
        break;
    case APP_PACKET_COMM_HEART_BEAT:
        if (!(pmg->ucMsgType & 0x80))
        {
            return CanCcbAfDataClientHeartBeatMsg(ucCcbIndex,(uint8_t *)pmg);
        }
    case APP_PACKET_COMM_HOST_RESET:
        if (!(pmg->ucMsgType & 0x80))
        {
            return CanCcbAfDataClientClientHostResetMsg(ucCcbIndex,(uint8_t *)pmg);
        }
        break;
    case APP_PACKET_CLIENT_REPORT:
        if ((pmg->ucMsgType & 0x80))
        {
            return CanCcbAfDataRptAckMsg(ucCcbIndex,(uint8_t *)pmg);
        }
        break;
    case APP_PACKET_EXE_OPERATION:
        if (!(pmg->ucMsgType & 0x80))
        {
            return CanCcbAfDataClientOpsMsg(ucCcbIndex,(uint8_t *)pmg);
        }
        break;
    default: // yet to be implemented
        break;
    }

    return 0;
}


uint8 CanCcbAfModbusReadInputRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusReadInputRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,pModbusRsp->aucData[0] + MODBUS_PACKET_COMM_HDR_LEN + 1); 

    return 0;
}

uint8 CanCcbAfModbusReadInputStatus(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusReadInputStatus(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,pModbusRsp->aucData[0] + MODBUS_PACKET_COMM_HDR_LEN + 1); 

    return 0;
}

uint8 CanCcbAfModbusReadCoilStatus(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusReadCoilStatus(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,pModbusRsp->aucData[0] + MODBUS_PACKET_COMM_HDR_LEN + 1); 

    return 0;
}

uint8 CanCcbAfModbusForceSingleCoil(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusForceSingleCoil(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 4); 

    return 0;
}

uint8 CanCcbAfModbusForceMultipleCoils(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusForceMultipleCoils(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 4); 

    return 0;
}


uint8 CanCcbAfModbusMaskWrite0XRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusMaskWrite0XRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 6); 

    return 0;
}

uint8 CanCcbAfModbusMaskWrite4XRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusMaskWrite4XRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 6); 

    return 0;
}

uint8 CanCcbAfModbusPresetSingleRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t buffer[20];

    /* PC RAW Test Command:  0428 06 0002 0010 */
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusPresetSingleRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 4); 

    return 0;
}

uint8 CanCcbAfModbusPresetMultipleRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t buffer[20];
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusPresetMultipleRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,MODBUS_PACKET_COMM_HDR_LEN + 4); 

    return 0;
}


uint8 CanCcbAfModbusReadHoldRegister(UINT8 ucCcbIndex,uint8_t *msg)
{
    uint8_t *buffer = Config_buff;
    
    MODBUS_PACKET_COMM_STRU *pModbusReq = (MODBUS_PACKET_COMM_STRU *)msg;

    MODBUS_PACKET_COMM_STRU *pModbusRsp = (MODBUS_PACKET_COMM_STRU *)buffer;

    pModbusRsp->ucAddress = pModbusReq->ucAddress;   
    pModbusRsp->ucFuncode = pModbusReq->ucFuncode;   
    
    Disp_ModbusReadHoldRegister(ucCcbIndex,pModbusReq,pModbusRsp);

    // push to queue
    CanSndSappCmd(ucCcbIndex ,SAPP_CMD_MODBUS|0X80,(uint8_t *)pModbusRsp,pModbusRsp->aucData[0] + MODBUS_PACKET_COMM_HDR_LEN + 1); 

    return 0;
}

uint8 CanCcbAfModbusMsg(UINT8 ucCcbIndex)
{
    MODBUS_PACKET_COMM_STRU *pmg = (MODBUS_PACKET_COMM_STRU *)&sbRxBuf[RPC_POS_DAT0]; 

    CanCcb[ucCcbIndex].ucModbusAddress = pmg->ucAddress;
    CanCcb[ucCcbIndex].ucModbusFunCode = pmg->ucFuncode;

    switch(pmg->ucFuncode)
    {
    case MODBUS_FUNC_CODE_Read_Input_Registers:
        CanCcbAfModbusReadInputRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Read_Input_Status:
        CanCcbAfModbusReadInputStatus(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Read_Coil_Status:
        CanCcbAfModbusReadCoilStatus(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Force_Single_Coil:
        CanCcbAfModbusForceSingleCoil(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Force_Multiple_Coils:
        CanCcbAfModbusForceMultipleCoils(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Mask_Write_0X_Register:
        CanCcbAfModbusMaskWrite0XRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Mask_Write_4X_Register:
        CanCcbAfModbusMaskWrite4XRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Preset_Single_Register:
        CanCcbAfModbusPresetSingleRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Preset_Multiple_Registers:
        CanCcbAfModbusPresetMultipleRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    case MODBUS_FUNC_CODE_Read_Holding_Registers:
        CanCcbAfModbusReadHoldRegister(ucCcbIndex,(uint8_t *)pmg);
        break;
    default: // yet to be implemented
        VOS_LOG(VOS_LOG_DEBUG, "unknow modbus code %x\r\n",pmg->ucFuncode);
        break;
    }

    return 0;
}



/**************************************************************************************************
 * @fn          CanCcbAfProc
 *
 * @brief       Process the SB command and received buffer.
 *
 * input parameters
 *
 * None.
 *
 * output parameters
 *
 * None.
 *
 * @return      None.
 */
 
uint8_t CanCcbAfProc(UINT8 ucCcbIndex)
{
    switch(sbRxBuf[RPC_POS_CMD1])
    {
    case SAPP_CMD_DATA:
        CanCcbAfDataMsg(ucCcbIndex);
        break;
    case SAPP_CMD_MODBUS:
        CanCcbAfModbusMsg(ucCcbIndex);
        break;
    }
    return FALSE;
}

void CanRcvFrame(UINT8 ucCcbIndex,UINT8 ucRcvBufIndex)
{
    UINT8 ucRet = TRUE;

    MainAlarmWithDuration(1);

    CanCcb[ucCcbIndex].usSrcAddr = CanRcvBuff[ucRcvBufIndex].usSrcAddr;

    if (SHZNAPP_CanParse(CanRcvBuff[ucRcvBufIndex].head,CanRcvBuff[ucRcvBufIndex].len))
    {
        CanHashAddr = CanAddress;

        sappItfType = Interface_CAN;

        switch(sbRxBuf[RPC_POS_CMD0])
        {
        case RPC_SYS_APP:
            ucRet = SHZNAPP_SerialAppProc();
            break;
        case RPC_SYS_BOOT:
            {
                uint8_t ucCmd1 = sbRxBuf[RPC_POS_CMD1];
                ucRet = SHZNAPP_SerialBootProc();
                if (SBL_QUERY_ID_CMD == ucCmd1)
                {
                    if (INVALID_CAN_ADDRESS(CanHashAddr))
                    {
                        // calc hash address
                        CanHashAddr = CanCmdHashAdr();
                    }
                }
            }
            break;
        case RPC_SYS_AF:
            ucRet = CanCcbAfProc(ucCcbIndex);
            break;
        default:
            ucRet = SHZNAPP_SerialUnknowProc();
   
            break;
        }

        if (ucRet)
        {
            (void)SHZNAPP_CanResp(CanRcvBuff[ucRcvBufIndex].usSrcAddr,CanHashAddr);  // Send the SB response setup in the sbRxBuf passed to sblProc().
        }
    }
}


int CanSndSappRawCmd2(uint16 usDstCanAdr,uint16 usSrcCanAdr,uint8_t ucSys,uint8_t ucCmd,uint8_t *data, uint8_t len)
{
    sbTxBuf[RPC_POS_LEN]  = len; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
    sbTxBuf[RPC_POS_CMD0] = ucSys;
    sbTxBuf[RPC_POS_CMD1] = ucCmd;//SAPP_CMD_DATA;
    
    memcpy(&sbTxBuf[RPC_POS_DAT0],data,len);

    return SHZNAPP_CanResp(usDstCanAdr,usSrcCanAdr);
}

int CanSndSappCmd(uint8_t ucCcbIdx,uint8_t ucCmd,uint8_t *data, uint8_t len)
{
    switch(ucCcbIdx)
    {
    default:
         {
            UINT16 srcAddress = (ucCcbIdx == CAN_LOCAL_FM_INDEX) ? CanAddress + 1 : CanAddress;
            sbTxBuf[RPC_POS_LEN]  = len; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
            sbTxBuf[RPC_POS_CMD0] = RPC_SYS_AF;
            sbTxBuf[RPC_POS_CMD1] = ucCmd;//SAPP_CMD_DATA;
            
            memcpy(&sbTxBuf[RPC_POS_DAT0],data,len);
            
            return SHZNAPP_CanResp(CanCcb[ucCcbIdx].usSrcAddr,srcAddress);
         }
    case CAN_ZIGBEE_INDEX:
         {
    
            int iOffset = RPC_POS_DAT0;
        
            sbTxBuf[RPC_POS_LEN]  = len + 3; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
            sbTxBuf[RPC_POS_CMD0] = RPC_SYS_APP;
            sbTxBuf[RPC_POS_CMD1] = ucCmd;//SAPP_CMD_DATA;
    
            sbTxBuf[iOffset++]    = afAddr16Bit;
            sbTxBuf[iOffset++]    = 0; /* to coo */
            sbTxBuf[iOffset++]    = 0;

            /* protol type */
            if (ucCmd != SAPP_CMD_AIR_DATA)
            {
                sbTxBuf[iOffset++]    = ucCmd;
                sbTxBuf[RPC_POS_CMD1] = SAPP_CMD_DATA;
                sbTxBuf[RPC_POS_LEN] += 1;
            }
            
            memcpy(&sbTxBuf[iOffset],data,len);
            
            /* to zigbee */
            zb_SerialResp();
         }
         break;
    }
	return 0;	    
}

int CanSndSappCmd2(uint8_t ucCcbIdx,uint16_t usDstCanAdr,uint16_t usSrcCanAdr,uint8_t ucCmd,uint8_t *data, uint8_t len)
{

    switch(ucCcbIdx)
    {
    default:
        {

            sbTxBuf[RPC_POS_LEN]  = len; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
            sbTxBuf[RPC_POS_CMD0] = RPC_SYS_AF;
            sbTxBuf[RPC_POS_CMD1] = ucCmd;//SAPP_CMD_DATA;
            
            memcpy(&sbTxBuf[RPC_POS_DAT0],data,len);
        
            return SHZNAPP_CanResp(usDstCanAdr,usSrcCanAdr);
        }
    case CAN_ZIGBEE_INDEX:
        {
   
           int iOffset = RPC_POS_DAT0;
       
           sbTxBuf[RPC_POS_LEN]  = len + 3; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
           sbTxBuf[RPC_POS_CMD0] = RPC_SYS_APP;
           sbTxBuf[RPC_POS_CMD1] = ucCmd;//SAPP_CMD_DATA;
   
           sbTxBuf[iOffset++]    = afAddr16Bit;
           sbTxBuf[iOffset++]    = 0; /* to coo */
           sbTxBuf[iOffset++]    = 0;

              /* protol type */
           if (ucCmd != SAPP_CMD_AIR_DATA)
           {
               sbTxBuf[iOffset++]    = ucCmd;
               sbTxBuf[RPC_POS_CMD1] = SAPP_CMD_DATA;
               sbTxBuf[RPC_POS_LEN] += 1;
           }
           
           memcpy(&sbTxBuf[iOffset],data,len);
           
           /* to zigbee */
           zb_SerialResp();
        }
         break;        
    }
		return 0;
}


/*********************************************************************
 * @fn      CanCmdPrintf
 *
 * @brief   format output to CAN bus
 *
 * @param   fmt  - format string
 * @param   ... - args
 * 
 * @return  none
 */
void CanCmdPrintf(uint16 usDstCanAdr,const char* fmt,...)
{
    char buf[64];
	va_list   args;

	va_start(args, fmt);
  
	vsprintf(buf,fmt,args);
  
	va_end(args);

    CanSndSappDbg(usDstCanAdr,(uint8_t *)buf,strlen(buf));
}

void CanSndSappDbg(uint16 usDstCanAdr,uint8_t *data, uint8_t len)
{
    sbTxBuf[RPC_POS_LEN]  = len; // len for data area (NOT INCLUDE CMD0&CMD1&LEN itself)
    sbTxBuf[RPC_POS_CMD0] = RPC_SYS_APP;
    sbTxBuf[RPC_POS_CMD1] = SAPP_CMD_DBG_INFO;//SAPP_CMD_DATA;
    
    memcpy(&sbTxBuf[RPC_POS_DAT0],data,len);

    SHZNAPP_CanResp(usDstCanAdr,CanAddress);
}

void CanCmdRegisterSndEmptyCallBack(CAN_Snd_Empty_CallBack cb)
{
    gCanSndEmptyCB = cb;
}

void CanCmd_msg_Handler(Message *Msg)
{
    CANCMD_MSG *dmsg = (CANCMD_MSG *)Msg;

    if (dmsg->para)
    {
        ((cancmd_msg_cb)dmsg->para)(dmsg->para1);
    }
}

void CanCcbRegister_msg_handler(void *para)
{
    // task
    UINT8 ucIndex = (UINT8)(UINT32)para;

    UINT8 aucBuffer[64];
    
    APP_PACKET_ONLINE_NOTI_IND_STRU *ind = (APP_PACKET_ONLINE_NOTI_IND_STRU *)aucBuffer;

    ind->hdr.ucMsgType   = APP_PAKCET_COMM_ONLINE_NOTI;

    switch(ucIndex)
    {
    case CAN_LOCAL_EXE_INDEX:
        {
            int iLoop;
            APP_PACKET_ONLINE_NOTI_IND_EXE_STRU *pCont = (APP_PACKET_ONLINE_NOTI_IND_EXE_STRU *)ind->acInfo;
            
            ind->hdr.ucLen       = sizeof(APP_PACKET_ONLINE_NOTI_IND_EXE_STRU);
            ind->hdr.ucTransId   = APP_DEV_TYPE_EXE_BOARD;
            ind->hdr.ucDevType   = APP_DEV_TYPE_EXE_BOARD;

            for (iLoop = 0; iLoop < APP_EXE_MAX_HOLD_REGISTERS; iLoop++)
            {
                pCont->ausHoldRegs[iLoop] = Display.ausHoldRegs[iLoop];
            }
        }
        break;
    case CAN_LOCAL_FM_INDEX:
        {
            int iLoop;
            APP_PACKET_ONLINE_NOTI_IND_FM_STRU *pCont = (APP_PACKET_ONLINE_NOTI_IND_FM_STRU *)ind->acInfo;
            
            ind->hdr.ucLen       = sizeof(APP_PACKET_ONLINE_NOTI_IND_FM_STRU);
            ind->hdr.ucTransId   = APP_DEV_TYPE_FLOW_METER;
            ind->hdr.ucDevType   = APP_DEV_TYPE_FLOW_METER;

            for (iLoop = 0; iLoop < APP_FM_FLOW_METER_NUM; iLoop++)
            {
                pCont->aulFm[iLoop] = Display.aulInputRegs_fm[iLoop];
            }
        }        
        break;
    default: /* CAN_ZIGBEE_INDEX */
        {
            int iLoop;
            APP_PACKET_ONLINE_NOTI_IND_EXE_COMBINED_STRU *pCont = (APP_PACKET_ONLINE_NOTI_IND_EXE_COMBINED_STRU *)ind->acInfo;
            
            ind->hdr.ucLen       = sizeof(APP_PACKET_ONLINE_NOTI_IND_EXE_COMBINED_STRU);
            ind->hdr.ucTransId   = APP_DEV_TYPE_EXE_COMBINED;
            ind->hdr.ucDevType   = APP_DEV_TYPE_EXE_COMBINED;

            for (iLoop = 0; iLoop < APP_EXE_MAX_HOLD_REGISTERS; iLoop++)
            {
                pCont->exe.ausHoldRegs[iLoop] = Display.ausHoldRegs[iLoop];
            }

            for (iLoop = 0; iLoop < APP_FM_FLOW_METER_NUM; iLoop++)
            {
                pCont->fm.aulFm[iLoop] = Display.aulInputRegs_fm[iLoop];
            }
        }        
        break;
    }
    
    // report device info to host
    AddTimer(TIMER_CCB_BEGIN + ucIndex,OS_TMR_OPT_ONE_SHOT,CAN_LOGIC_DEVICE_REGISETER_PERIOD/1000*OS_TMR_CFG_TICKS_PER_SEC,0XFFFF);
    
    CanCcb[ucIndex].ucMachineState = MACHINE_STATE_MAIN_WAIT_REGISTER_RSP;

    // set to main controller's address
    CanCcb[ucIndex].usSrcAddr = 0X1; 

    // push to queue
    CanSndSappCmd(ucIndex ,SAPP_CMD_DATA,(uint8_t *)ind,APP_PROTOL_HEADER_LEN + ind->hdr.ucLen); 
}

void CanCcb_Register(void *para)
{
     CanCmd_report(CanCcbRegister_msg_handler,para);
}

void CanCcbDeviceCommDeviceInfoRegister(UINT8 ucIndex)
{

    if (MACHINE_STATE_IDLE != CanCcb[ucIndex].ucMachineState)
    {
        // Ongoing procedure is waiting response from network controller
        // discard the message
        return;
    }

    if (CanCcb[ucIndex].bit1Registered)
    {
        return ; 
    }

    system_timeout(CAN_CMD_RSP_DELAY(ucIndex,20),SYS_TIMER_ONE_SHOT,CAN_CMD_RSP_DELAY(ucIndex,20),CanCcb_Register,(void *)ucIndex,&CanCcb[ucIndex].to4DelayExcu);

}



/*********************************************************************
 * Function:        void CanCcbSystemStartReportRegister(void)
 *
 * PreCondition:    None
 *
 * Input:           None
 *
 * Output:          None
 *
 * Side Effects:    None
 *
 * Overview:        Call Once when system is up to notify net controller of local setting 
 *
 * Note:            None.
 ********************************************************************/
void CanCcbSystemStartReportRegister(uint8_t ucTrxIndex)
{
    if (INVALID_CAN_ADDRESS(CanAddress)) 
    {
        return;
    }
    CanCcbDeviceCommDeviceInfoRegister(ucTrxIndex);

}


uint8_t CanCcbSndZigbeeIndMsg(void)
{
    uint8_t buf[16];
    
    APP_PACKET_ZIGBEE_IND_STRU     *pLoad = (APP_PACKET_ZIGBEE_IND_STRU *)buf;

    if (INVALID_CAN_ADDRESS(CanAddress))
    {
        return 0XFF;
    }
    
    pLoad->hdr.ucLen     = 2;
    pLoad->hdr.ucTransId = 0;
    pLoad->hdr.ucDevType = APP_DEV_TYPE_HAND_SET;
    pLoad->hdr.ucMsgType = APP_PACKET_COMM_ZIGBEE_IND;

    pLoad->usAddress    = CanAddress;

    // set to main controller's address
    CanCcb[CAN_ZIGBEE_INDEX].usSrcAddr = APP_DEV_TYPE_MAIN_CTRL; 

    // push to queue
    return CanSndSappCmd(CAN_ZIGBEE_INDEX ,SAPP_CMD_DATA,buf,pLoad->hdr.ucLen + APP_PROTOL_HEADER_LEN);
}

/* Callback */

void CanCcbSecondTask(void)
{
    CanCcbSystemStartReportRegister(CAN_LOCAL_EXE_INDEX);
    CanCcbDeviceCommDeviceInfoRegister(CAN_LOCAL_FM_INDEX);
    
    CanBusyCheck();

}

