#if 0
    FLI CCD
    INDI Interface for Finger Lakes Instruments CCDs
    Copyright (C) 2003 Jasem Mutlaq (mutlaqja@ikarustech.com)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <zlib.h>

#include "fli/libfli.h"
#include "fitsrw.h"
#include "indidevapi.h"
#include "eventloop.h"
#include "indicom.h"

void ISInit(void);
void getBasicData(void);
void ISPoll(void *);
void handleExposure(void *);
void connectCCD(void);
void getBasicData(void);
void initDataChannel(void);
void waitForData(int rp, int wp);
void updateDataChannel(void *p);
void uploadFile(char * filename);
int  findPort(void);
int  writeFITS(char *filename, char errmsg[]);
int  findcam(flidomain_t domain);
int  setImageArea(char errmsg[]);
int  manageDefaults(char errmsg[]);
int  grabImage(void);
int  findPort(void);
int  checkPowerS(ISwitchVectorProperty *sp);
int  checkPowerN(INumberVectorProperty *np);
int  checkPowerT(ITextVectorProperty *tp);
int  getOnSwitch(ISwitchVectorProperty *sp);
int  isCCDConnected(void);

double min(void);
double max(void);
FITS_HDU_LIST * create_fits_header (FITS_FILE *ofp, uint width, uint height, uint bpp);

extern char* me;
extern int errno;

#define mydev           "FLI CCD"

#define COMM_GROUP	"Communication"
#define EXPOSE_GROUP	"Expose"
#define IMAGE_GROUP	"Image Settings"
#define DATA_GROUP      "Data Channel"

#define MAX_CCD_TEMP	45		/* Max CCD temperature */
#define MIN_CCD_TEMP	-55		/* Min CCD temperature */
#define MAX_X_BIN	16.		/* Max Horizontal binning */
#define MAX_Y_BIN	16.		/* Max Vertical binning */
#define MAX_PIXELS	4096		/* Max number of pixels in one dimension */
#define POLLMS		1000		/* Polling time (ms) */
#define TEMP_THRESHOLD  .25		/* Differential temperature threshold (C)*/
#define NFLUSHES	1		/* Number of times a CCD array is flushed before an exposure */

#define FILENAMESIZ	2048
#define LIBVERSIZ 	1024
#define PREFIXSIZ	64
#define PIPEBUFSIZ	8192
#define FRAME_ILEN	64

#define getBigEndian(p) ( ((p & 0xff) << 8) | (p  >> 8))

enum FLIFrames { LIGHT_FRAME = 0, BIAS_FRAME, DARK_FRAME, FLAT_FRAME };


typedef struct {
  flidomain_t domain;
  char *dname;
  char *name;
  char *model;
  long HWRevision;
  long FWRevision;
  double x_pixel_size;
  double y_pixel_size;
  long Array_Area[4];
  long Visible_Area[4];
  int width, height;
  double temperature;
} cam_t;

typedef struct {
int  width;
int  height;
int  frameType;
int  expose;
unsigned short  *img;
} img_t;

typedef struct {
 int rp;				/* Read pipe */
 int wp;				/* Write pipe */
 int streamReady;			/* Can we write to data stream? */
} client_t;

client_t *INDIClients;			/* INDI clients */
static int DataPort;			/* Data Port */
static int streamTimerID;		/* Stream ID */
static unsigned int nclients;		/* # of clients using the binary stream */

static flidev_t fli_dev;
static cam_t *FLICam;
static img_t *FLIImg;
static int portSwitchIndex;
static int imageCount; 

long int Domains[] = { FLIDOMAIN_USB, FLIDOMAIN_SERIAL, FLIDOMAIN_PARALLEL_PORT,  FLIDOMAIN_INET };

/*INDI controls */

/* Connect/Disconnect */
static ISwitch PowerS[]          	= {{"CONNECT" , "Connect" , ISS_OFF, 0, 0},{"DISCONNECT", "Disconnect", ISS_ON, 0, 0}};
static ISwitchVectorProperty PowerSP	= { mydev, "CONNECTION" , "Connection", COMM_GROUP, IP_RW, ISR_1OFMANY, 60, IPS_IDLE, PowerS, NARRAY(PowerS), 0, 0};

/* Types of Ports */
static ISwitch PortS[]           	= {{"USB", "", ISS_ON, 0, 0}, {"Serial", "", ISS_OFF, 0, 0}, {"Parallel", "", ISS_OFF, 0, 0}, {"INet", "", ISS_OFF, 0, 0}};
static ISwitchVectorProperty PortSP	= { mydev, "Port Type", "", COMM_GROUP, IP_RW, ISR_1OFMANY, 0, IPS_IDLE, PortS, NARRAY(PortS), 0, 0};

/* Types of Frames */
static ISwitch FrameTypeS[]		= { {"FRAME_LIGHT", "Light", ISS_ON, 0, 0}, {"FRAME_BIAS", "Bias", ISS_OFF, 0, 0}, {"FRAME_DARK", "Dark", ISS_OFF, 0, 0}, {"FRAME_FLAT", "Flat Field", ISS_OFF, 0, 0}};
static ISwitchVectorProperty FrameTypeSP = { mydev, "FRAME_TYPE", "Frame Type", EXPOSE_GROUP, IP_RW, ISR_1OFMANY, 0, IPS_IDLE, FrameTypeS, NARRAY(FrameTypeS), 0, 0};

/* Frame coordinates. Full frame is default */
static INumber FrameN[]          	= {
 { "X", "", "%.0f", 0.,     MAX_PIXELS, 1., 0., 0, 0, 0},
 { "Y", "", "%.0f", 0.,     MAX_PIXELS, 1., 0., 0, 0, 0},
 { "Width", "", "%.0f", 0., MAX_PIXELS, 1., 0., 0, 0, 0},
 { "Height", "", "%.0f",0., MAX_PIXELS, 1., 0., 0, 0, 0}};
 static INumberVectorProperty FrameNP = { mydev, "FRAME", "Frame", IMAGE_GROUP, IP_RW, 60, IPS_IDLE, FrameN, NARRAY(FrameN), 0, 0};
 
 /* Binning */ 
 static INumber BinningN[]       = {
 { "HOR_BIN", "X", "%0.f", 1., MAX_X_BIN, 1., 1., 0, 0, 0},
 { "VER_BIN", "Y", "%0.f", 1., MAX_Y_BIN, 1., 1., 0, 0, 0}};
 static INumberVectorProperty BinningNP = { mydev, "BINNING", "Binning", IMAGE_GROUP, IP_RW, 60, IPS_IDLE, BinningN, NARRAY(BinningN), 0, 0};
 
 /* Exposure time */
  static INumber ExposeTimeN[]    = {{ "EXPOSE_S", "Duration (s)", "%5.2f", 0., 36000., .5, 1., 0, 0, 0}};
  static INumberVectorProperty ExposeTimeNP = { mydev, "EXPOSE_DURATION", "Expose", EXPOSE_GROUP, IP_RW, 60, IPS_IDLE, ExposeTimeN, NARRAY(ExposeTimeN), 0, 0};
 
  /* Temperature control */
 static INumber TemperatureN[]	  = { {"TEMPERATURE", "Temperature", "%+06.2f", MIN_CCD_TEMP, MAX_CCD_TEMP, .2, 0., 0, 0, 0}};
 static INumberVectorProperty TemperatureNP = { mydev, "CCD_TEMPERATURE", "Temperature (C)", EXPOSE_GROUP, IP_RW, 60, IPS_IDLE, TemperatureN, NARRAY(TemperatureN), 0, 0};
 
 /* Expose progress */
 static INumber ExposeProgressN[] = { {"Time left", "", "%.0f", 0., 0., 0., 0., 0, 0, 0} };
 static INumberVectorProperty ExposeProgressNP  = { mydev, "Expose Progress (s)", "", EXPOSE_GROUP, IP_RO, 0, IPS_IDLE, ExposeProgressN, NARRAY(ExposeProgressN), 0, 0};
  
 /* Pixel size (µm) */
static INumber PixelSizeN[] 	= {
	{ "Width", "", "%.0f", 0. , 0., 0., 0., 0, 0, 0},
	{ "Height", "", "%.0f", 0. , 0., 0., 0., 0, 0, 0}};
static INumberVectorProperty PixelSizeNP = { mydev, "Pixel Size (µm)", "", DATA_GROUP, IP_RO, 0, IPS_IDLE, PixelSizeN, NARRAY(PixelSizeN), 0, 0};

 /* Data channel */
static INumber DataChannelN[]		= {{"CHANNEL", "Channel", "%0.f", 1024., 20000., 1., 0., 0, 0, 0}};
static INumberVectorProperty DataChannelNP={ mydev, "DATA_CHANNEL", "Data Channel", DATA_GROUP, IP_RO, 0, IPS_IDLE, DataChannelN, NARRAY(DataChannelN), 0, 0};

/* send client definitions of all properties */
void ISInit()
{
  static int isInit=0;

 if (isInit)
  return;
 
 /* USB by default {USB, SERIAL, PARALLEL, INET} */
 portSwitchIndex = 0;
 
 FLIImg = malloc (sizeof(img_t));
 
 if (FLIImg == NULL)
 {
   IDMessage(mydev, "Error: unable to initialize driver. Low memory.");
   IDLog("Error: unable to initialize driver. Low memory.");
   return;
 }
 
 imageCount = 0;
 streamTimerID = -1;
 INDIClients = NULL;
 nclients    = 0;
 
 isInit = 1;
 
}

void ISGetProperties (const char *dev)
{ 

   ISInit();
  
  if (dev && strcmp (mydev, dev))
    return;

  /* COMM_GROUP */
  IDDefSwitch(&PowerSP, NULL);
  IDDefSwitch(&PortSP, NULL);
  
  /* Expose */
  IDDefSwitch(&FrameTypeSP, NULL);  
  IDDefNumber(&ExposeTimeNP, NULL);
  IDDefNumber(&TemperatureNP, NULL);
  
  /* Image Group */
  IDDefNumber(&FrameNP, NULL);
  IDDefNumber(&BinningNP, NULL);
  
  /* Data Group */
  IDDefNumber (&DataChannelNP, NULL);
  
  INDIClients = INDIClients ? (client_t *) realloc(INDIClients, (nclients+1) * sizeof(client_t)) :
                              (client_t *) malloc (sizeof(client_t));
    
  if (INDIClients)
  {
        INDIClients[nclients].streamReady = 0;
	INDIClients[nclients].rp	  = 0;
	INDIClients[nclients].wp          = 0;
  	DataPort = findPort();
        if (DataPort > 0)
 		initDataChannel();

        /* Send the basic data to the new client if the previous client(s) are already connected. */		
	if (PowerSP.s == IPS_OK)
	  getBasicData();
  }
  
 
  	
  IEAddTimer (POLLMS, ISPoll, NULL);
  
}
  
void ISNewSwitch (const char *dev, const char *name, ISState *states, char *names[], int n)
{
	long err;
	int i;
	ISwitch *sp;
	
	/* ignore if not ours */
	if (dev && strcmp (dev, mydev))
	    return;
	    
	ISInit();
	    
	/* Port type */
	if (!strcmp (name, PortSP.name))
	{
   	  PortSP.s = IPS_IDLE; 
	  IUResetSwitches(&PortSP);
	  IUUpdateSwitches(&PortSP, states, names, n);
	  portSwitchIndex = getOnSwitch(&PortSP);
	  
	  PortSP.s = IPS_OK; 
	  IDSetSwitch(&PortSP, NULL);
	  return;
	}
	
	/* Connection */
	if (!strcmp (name, PowerSP.name))
	{
	  IUResetSwitches(&PowerSP);
	  IUUpdateSwitches(&PowerSP, states, names, n);
   	  connectCCD();
	  return;
	}
	
     /* Frame Type */
     if (!strcmp(FrameTypeSP.name, name))
     {
       if (checkPowerS(&FrameTypeSP))
         return;
	 
       FrameTypeSP.s = IPS_IDLE;
	 
       for (i = 0; i < n ; i++)
       {
         sp = IUFindSwitch(&FrameTypeSP, names[i]);
	 
	 if (!sp)
	 {
	      IDSetSwitch(&FrameTypeSP, "Unknown error. %s is not a member of %s property.", names[0], name);
	      return;
	 }
	 
	 /* NORMAL, BIAS, or FLAT */
	 if ( (sp == &FrameTypeS[LIGHT_FRAME] || sp == &FrameTypeS[FLAT_FRAME]) && states[i] == ISS_ON)
	 {
	   if (sp == &FrameTypeS[LIGHT_FRAME])
	     FLIImg->frameType = LIGHT_FRAME;
  	   else
	     FLIImg->frameType = FLAT_FRAME;
	   
	   if ((err = FLISetFrameType(fli_dev, FLI_FRAME_TYPE_NORMAL) ))
  	   {
	    IUResetSwitches(&FrameTypeSP);
	    FrameTypeS[LIGHT_FRAME].s = ISS_ON;
            IDSetSwitch(&FrameTypeSP, "FLISetFrameType() failed. %s.\n", strerror((int)-err));
	    IDLog("FLISetFrameType() failed. %s.\n", strerror((int)-err));
	    return;
	   }
	  
	   IUResetSwitches(&FrameTypeSP);
	   sp->s = ISS_ON; 
	   FrameTypeSP.s = IPS_OK;
	   IDSetSwitch(&FrameTypeSP, NULL);
	   break;
	 }
	 /* DARK AND BIAS */
	 else if ( (sp == &FrameTypeS[DARK_FRAME] || sp == &FrameTypeS[BIAS_FRAME]) && states[i] == ISS_ON)
	 {
	   
	  if (sp == &FrameTypeS[DARK_FRAME])
	     FLIImg->frameType = DARK_FRAME;
	  else
	     FLIImg->frameType = BIAS_FRAME;
	   
	   if ((err = FLISetFrameType(fli_dev, FLI_FRAME_TYPE_DARK) ))
  	   {
	    IUResetSwitches(&FrameTypeSP);
	    FrameTypeS[LIGHT_FRAME].s = ISS_ON;
            IDSetSwitch(&FrameTypeSP, "FLISetFrameType() failed. %s.\n", strerror((int)-err));
	    IDLog("FLISetFrameType() failed. %s.\n", strerror((int)-err));
	    return;
	   }
	   
	   IUResetSwitches(&FrameTypeSP);
	   sp->s = ISS_ON;
	   FrameTypeSP.s = IPS_OK;
	   IDSetSwitch(&FrameTypeSP, NULL);
	   break;
	 }
	   
  	} /* For loop */
	
	return;
     }
     
}

void ISNewText (const char *dev, const char *name, char *texts[], char *names[], int n)
{
	ISInit();
 
       /* ignore if not ours */ 
       if (dev && strcmp (mydev, dev))
         return;

	/* suppress warning */
	n=n; dev=dev; name=name; names=names; texts=texts;
	
}


void ISNewNumber (const char *dev, const char *name, double values[], char *names[], int n)
{
      long err;
      int i;
      INumber *np;
      char errmsg[1024];

	/* ignore if not ours */
	if (dev && strcmp (dev, mydev))
	    return;
	    
	ISInit();
	
    /* Exposure time */
    if (!strcmp (ExposeTimeNP.name, name))
    {
       if (checkPowerN(&ExposeTimeNP))
         return;

       if (ExposeTimeNP.s == IPS_BUSY)
       {
          if ( (err = FLICancelExposure(fli_dev)))
	  {
	    	ExposeTimeNP.s = IPS_IDLE;
	    	IDSetNumber(&ExposeTimeNP, "FLICancelExposure() failed. %s.", strerror((int)-err));
	    	IDLog("FLICancelExposure() failed. %s.\n", strerror((int)-err));
	    	return;
	  }
	
	  ExposeTimeNP.s = IPS_IDLE;
	  ExposeTimeN[0].value = 0;

	  IDSetNumber(&ExposeTimeNP, "Exposure cancelled.");
	  IDLog("Exposure Cancelled.\n");
	  return;
        }
    
       ExposeTimeNP.s = IPS_IDLE;
       
       np = IUFindNumber(&ExposeTimeNP, names[0]);
	 
       if (!np)
       {
	   IDSetNumber(&ExposeTimeNP, "Error: %s is not a member of %s property.", names[0], name);
	   return;
       }
	 
        np->value = values[0];
	FLIImg->expose = (int) (values[0] * 1000.);
	
      /* Set duration */  
     if ( (err = FLISetExposureTime(fli_dev, np->value * 1000.) ))
     {
       IDSetNumber(&ExposeTimeNP, "FLISetExposureTime() failed. %s.\n", strerror((int)-err));
       IDLog("FLISetExposureTime() failed. %s.\n", strerror((int)-err));
       return;
     }
      
      IDLog("Exposure Time (ms) is: %g\n", np->value * 1000.);
      
      handleExposure(NULL);
      return;
    } 
    
    
  if (!strcmp(TemperatureNP.name, name))
  {
    if (checkPowerN(&TemperatureNP))
      return;
      
    TemperatureNP.s = IPS_IDLE;
    
    np = IUFindNumber(&TemperatureNP, names[0]);
    
    if (!np)
    {
    IDSetNumber(&TemperatureNP, "Unknown error. %s is not a member of %s property.", names[0], name);
    return;
    }
    
    if (values[0] < MIN_CCD_TEMP || values[0] > MAX_CCD_TEMP)
    {
      IDSetNumber(&TemperatureNP, "Error: valid range of temperature is from %d to %d", MIN_CCD_TEMP, MAX_CCD_TEMP);
      return;
    }
    
    if ( (err = FLISetTemperature(fli_dev, values[0])))
    {
      IDSetNumber(&TemperatureNP, "FLISetTemperature() failed. %s.", strerror((int)-err));
      IDLog("FLISetTemperature() failed. %s.", strerror((int)-err));
      return;
    }
    
    FLICam->temperature = values[0];
    TemperatureNP.s = IPS_BUSY;
    
    IDSetNumber(&TemperatureNP, "Setting CCD temperature to %+06.2f C", values[0]);
    IDLog("Setting CCD temperature to %+06.2f C\n", values[0]);
    return;
   }
   
   if (!strcmp(FrameNP.name, name))
   {
     int nset=0;
     
     if (checkPowerN(&FrameNP))
      return;
      
     FrameNP.s = IPS_IDLE;
     
     for (i=0; i < n ; i++)
     {
       np = IUFindNumber(&FrameNP, names[i]);
       
      if (!np)
      {
       IDSetNumber(&FrameNP, "Unknown error. %s is not a member of %s property.", names[0], name);
       return;
      }
      
      /* X or Width */
      if (np == &FrameN[0] || np==&FrameN[2])
      {
        if (values[i] < 0 || values[i] > FLICam->width)
	 break;
	 
	nset++;
	np->value = values[i];
      }
      /* Y or height */
      else if (np == &FrameN[1] || np==&FrameN[3])
      {
       if (values[i] < 0 || values[i] > FLICam->height)
	 break;
	 
	nset++;
	np->value = values[i];
      }
     }
      
      if (nset < 4)
      {
        IDSetNumber(&FrameNP, "Invalid range. Valid range is (0,0) - (%0.f,%0.f)", FLICam->width, FLICam->height);
	IDLog("Invalid range. Valid range is (0,0) - (%0.f,%0.f)", FLICam->width, FLICam->height);
	return; 
      }
      
      if (setImageArea(errmsg))
      {
        IDSetNumber(&FrameNP, errmsg);
	return;
      }
      	    
      FrameNP.s = IPS_OK;
      
      /* Adjusting image width and height */ 
      FLIImg->width  = FrameN[2].value;
      FLIImg->height = FrameN[3].value;
      
      IDSetNumber(&FrameNP, NULL);
      
   } /* end FrameNP */
      
    
   if (!strcmp(BinningNP.name, name))
   {
     if (checkPowerN(&BinningNP))
       return;
       
     BinningNP.s = IPS_IDLE;
     
     for (i=0 ; i < n ; i++)
     {
       np = IUFindNumber(&BinningNP, names[i]);
       
      if (!np)
      {
       IDSetNumber(&BinningNP, "Unknown error. %s is not a member of %s property.", names[0], name);
       return;
      }
      
      /* X binning */
      if (np == &BinningN[0])
      {
        if (values[i] < 1 || values[i] > MAX_X_BIN)
	{
	  IDSetNumber(&BinningNP, "Error: Valid X bin values are from 1 to %d", MAX_X_BIN);
	  IDLog("Error: Valid X bin values are from 1 to %d", MAX_X_BIN);
	  return;
	}
	
	if ( (err = FLISetHBin(fli_dev, values[i])))
	{
	  IDSetNumber(&BinningNP, "FLISetHBin() failed. %s.", strerror((int)-err));
	  IDLog("FLISetHBin() failed. %s.", strerror((int)-err));
	  return;
	}
	
	np->value = values[i];
      }
      else if (np == &BinningN[1])
      {
        if (values[i] < 1 || values[i] > MAX_Y_BIN)
	{
	  IDSetNumber(&BinningNP, "Error: Valid Y bin values are from 1 to %d", MAX_Y_BIN);
	  IDLog("Error: Valid X bin values are from 1 to %d", MAX_Y_BIN);
	  return;
	}
	
	if ( (err = FLISetVBin(fli_dev, values[i])))
	{
	  IDSetNumber(&BinningNP, "FLISetVBin() failed. %s.", strerror((int)-err));
	  IDLog("FLISetVBin() failed. %s.", strerror((int)-err));
	  return;
	}
	
	np->value = values[i];
      }
     } /* end for */
     
     if (setImageArea(errmsg))
     {
       IDSetNumber(&BinningNP, errmsg, NULL);
       IDLog(errmsg);
       return;
     }
     
     BinningNP.s = IPS_OK;
     
     IDLog("Binning is: %.0f x %.0f\n", BinningN[0].value, BinningN[1].value);
     
     IDSetNumber(&BinningNP, NULL);
     return;
   }
	
}


void ISPoll(void *p)
{
	long err;
	long timeleft;
	double ccdTemp;
	
	if (!isCCDConnected())
	{
	 IEAddTimer (POLLMS, ISPoll, NULL);
	 return;
	}
	 
        /*IDLog("In Poll.\n");*/
	
	switch (ExposeTimeNP.s)
	{
	  case IPS_IDLE:
	    break;
	    
	  case IPS_OK:
	    break;
	    
	  case IPS_BUSY:
	    if ( (err = FLIGetExposureStatus(fli_dev, &timeleft)))
	    { 
	      ExposeTimeNP.s = IPS_IDLE; 
	      ExposeTimeN[0].value = 0;
	      
	      IDSetNumber(&ExposeTimeNP, "FLIGetExposureStatus() failed. %s.", strerror((int)-err));
	      IDLog("FLIGetExposureStatus() failed. %s.\n", strerror((int)-err));
	      break;
	    }
	    
	    /*ExposeProgressN[0].value = (timeleft / 1000.);*/
	    
	    if (timeleft > 0)
	    {
	      ExposeTimeN[0].value = timeleft / 1000.;
	      IDSetNumber(&ExposeTimeNP, NULL); 
	      break;
	    }
	    /*{
	      IDSetNumber(&ExposeProgressNP, NULL);
	      break;
	    }*/
	    
	    /* We're done exposing */
	    ExposeTimeNP.s = IPS_IDLE; 
	    ExposeTimeN[0].value = 0;
	    /*ExposeProgressNP.s = IPS_IDLE;*/
	    IDSetNumber(&ExposeTimeNP, "Exposure done, downloading image...");
	    IDLog("Exposure done, downloading image...\n");
	    /*IDSetNumber(&ExposeProgressNP, NULL);*/
	    
	    /* grab and save image */
	     if (grabImage())
	      break;
	    
	    /* Multiple image exposure 
	    if ( imagesLeft > 0)
	    { 
	      IDMessage(mydev, "Image #%d will be taken in %0.f seconds.", imageCount+1, DelayN[0].value);
	      IDLog("Image #%d will be taken in %0.f seconds.", imageCount+1, DelayN[0].value);
	      IEAddTimer (DelayN[0].value * 1000., handleExposure, NULL);
	    }*/
	    break;
	    
	  case IPS_ALERT:
	    break;
	 }
	 
	 switch (ExposeProgressNP.s)
	 {
	   case IPS_IDLE:
	   case IPS_OK:
	   	break;
           case IPS_BUSY:
	      ExposeProgressN[0].value--;
	      
	      if (ExposeProgressN[0].value > 0)
	          IDSetNumber(&ExposeProgressNP, NULL);
	      else
	      {
	        ExposeProgressNP.s = IPS_IDLE;
		IDSetNumber(&ExposeProgressNP, NULL);
	      }
	      break;
	      
	  case IPS_ALERT:
	     break;
	  }
		
	 
	 switch (TemperatureNP.s)
	 {
	   case IPS_IDLE:
	   case IPS_OK:
	     if ( (err = FLIGetTemperature(fli_dev, &ccdTemp)))
	     {
	       TemperatureNP.s = IPS_IDLE;
	       IDSetNumber(&TemperatureNP, "FLIGetTemperature() failed. %s.", strerror((int)-err));
	       IDLog("FLIGetTemperature() failed. %s.", strerror((int)-err));
	       return;
	     }
	     
	     if (fabs(TemperatureN[0].value - ccdTemp) >= TEMP_THRESHOLD)
	     {
	       TemperatureN[0].value = ccdTemp;
	       IDSetNumber(&TemperatureNP, NULL);
	     }
	     break;
	     
	   case IPS_BUSY:
	   if ((err = FLIGetTemperature(fli_dev, &ccdTemp)))
	     {
	       TemperatureNP.s = IPS_ALERT;
	       IDSetNumber(&TemperatureNP, "FLIGetTemperature() failed. %s.", strerror((int)-err));
	       IDLog("FLIGetTemperature() failed. %s.", strerror((int)-err));
	       return;
	     }
	     
	     if (fabs(FLICam->temperature - ccdTemp) <= TEMP_THRESHOLD)
	       TemperatureNP.s = IPS_OK;
	     	       
              TemperatureN[0].value = ccdTemp;
	      IDSetNumber(&TemperatureNP, NULL);
	      break;
	      
	    case IPS_ALERT:
	     break;
	  }
  	 
	 p=p; 
 	
	 IEAddTimer (POLLMS, ISPoll, NULL);
}

/* Sets the Image area that the CCD will scan and download.
   We compensate for binning. */
int setImageArea(char errmsg[])
{
  
   long x_1, y_1, x_2, y_2;
   long err;
   
   /* Add the X and Y offsets */
   x_1 = FrameN[0].value + FLICam->Visible_Area[0];
   y_1 = FrameN[1].value + FLICam->Visible_Area[1];
   
   x_2 = x_1 + (FrameN[2].value / BinningN[0].value);
   y_2 = y_1 + (FrameN[3].value / BinningN[1].value);
   
   if (x_2 > FLICam->Visible_Area[2])
     x_2 = FLICam->Visible_Area[2];
   
   if (y_2 > FLICam->Visible_Area[3])
     y_2 = FLICam->Visible_Area[3];
     
   IDLog("The Final image area is (%ld, %ld), (%ld, %ld)\n", x_1, y_1, x_2, y_2);
   
   FLIImg->width  = x_2 - x_1;
   FLIImg->height = y_2 - y_1;
   
   if ( (err = FLISetImageArea(fli_dev, x_1, y_1, x_2, y_2) ))
   {
     sprintf(errmsg, "FLISetImageArea() failed. %s.\n", strerror((int)-err));
     IDLog(errmsg, NULL);
     return -1;
   }
   
   return 0;
}

/* Downloads the image from the CCD row by row and store them
   in a raw file.
 N.B. No processing is done on the image */
int grabImage()
{
  long err;
  int img_size,i, fd;
  char errmsg[1024];
  char filename[] = "/tmp/fitsXXXXXX";
  
   if ((fd = mkstemp(filename)) < 0)
   { 
    IDMessage(mydev, "Error making temporary filename.", NULL);
    IDLog("Error making temporary filename.\n", NULL);
    return -1;
   }
   close(fd);
     
  img_size = FLIImg->width * FLIImg->height * sizeof(unsigned short);
  
  FLIImg->img = malloc (img_size);
  
  if (FLIImg->img == NULL)
  {
    IDMessage(mydev, "Not enough memory to store image.", NULL);
    IDLog("Not enough memory to store image.\n", NULL);
    return -1;
  }
  
  for (i=0; i < FLIImg->width ; i++)
  {
    if ( (err = FLIGrabRow(fli_dev, &FLIImg->img[i * FLIImg->width], FLIImg->width)))
    {
       free(FLIImg->img);
       IDMessage(mydev, "FLIGrabRow() failed at row %d. %s.", i, strerror((int)-err));
       IDLog("FLIGrabRow() failed at row %d. %s.\n", i, strerror((int)-err));
       return -1;
    }
  }
  
   /*err = (ImageFormatS[0].s == ISS_ON) ? writeFITS(FileNameT[0].text, errmsg) : writeRAW(FileNameT[0].text, errmsg);*/
   err = writeFITS(filename, errmsg);
   
   if (err)
   {
       free(FLIImg->img);
       IDMessage(mydev, errmsg, NULL);
       return -1;
   }
   
  free(FLIImg->img);
  return 0;
 
}

int writeFITS(char *filename, char errmsg[])
{
  FITS_FILE* ofp;
  int i, j, bpp, bpsl, width, height;
  long nbytes;
  FITS_HDU_LIST *hdu;
  
  ofp = fits_open (filename, "w");
  if (!ofp)
  {
    sprintf(errmsg, "Error: cannot open file for writing.");
    return (-1);
  }
  
  width  = FLIImg->width;
  height = FLIImg->height;
  bpp    = sizeof(unsigned short); /* Bytes per Pixel */
  bpsl   = bpp * FLIImg->width;    /* Bytes per Line */
  nbytes = 0;
  
  hdu = create_fits_header (ofp, width, height, bpp);
  if (hdu == NULL)
  {
     sprintf(errmsg, "Error: creating FITS header failed.");
     return (-1);
  }
  if (fits_write_header (ofp, hdu) < 0)
  {
    sprintf(errmsg, "Error: writing to FITS header failed.");
    return (-1);
  }
  
  /* Convert buffer to BIG endian */
  for (i=0; i < FLIImg->height; i++)
    for (j=0 ; j < FLIImg->width; j++)
      FLIImg->img[FLIImg->width * i + j] = getBigEndian( (FLIImg->img[FLIImg->width * i + j]) );
  
  for (i= 0; i < FLIImg->height  ; i++)
  {
    fwrite(FLIImg->img + (i * FLIImg->width), 2, FLIImg->width, ofp->fp);
    nbytes += bpsl;
  }
  
  nbytes = nbytes % FITS_RECORD_SIZE;
  if (nbytes)
  {
    while (nbytes++ < FITS_RECORD_SIZE)
      putc (0, ofp->fp);
  }
  
  if (ferror (ofp->fp))
  {
    sprintf(errmsg, "Error: write error occured");
    return (-1);
  }
 
 fits_close (ofp);      
 
  /* Success */
 ExposeTimeNP.s = IPS_OK;
 /*IDSetNumber(&ExposeTimeNP, "FITS image written to %s", filename);
 IDLog("FITS image written to '%s'\n", filename);*/
 IDSetNumber(&ExposeTimeNP, NULL);
 IDLog("Loading FITS image...\n");
 
 uploadFile(filename);
 
 return 0;

}

void uploadFile(char * filename)
{
   FILE * fitsFile;
   char frameSize[FRAME_ILEN];
   unsigned char *fitsData, *compressedData;
   int n=0,r=0;
   unsigned int i =0, j = 0, nr = 0, frameLen;
   uLongf compressedBytes=0;
   uLong  totalBytes;
   struct stat stat_p; 
 
   if ( -1 ==  stat (filename, &stat_p))
   { 
     IDLog(" Error occoured attempting to stat %s\n", filename); 
     return; 
   }
   
   totalBytes     = stat_p.st_size;
   fitsData       = (unsigned char *) malloc (sizeof(unsigned char) * totalBytes);
   compressedData = (unsigned char *) malloc (sizeof(unsigned char) * totalBytes + totalBytes / 64 + 16 + 3);
   
   if (fitsData == NULL || compressedData == NULL)
   {
     IDLog("Error! low memory. Unable to initialize fits buffers.\n");
     return;
   }
   
   fitsFile = fopen(filename, "r");
   
   if (fitsFile == NULL)
    return;
   
   /* #1 Read file from disk */ 
   for (i=0; i < totalBytes; i+= nr)
   {
      nr = fread(fitsData + i, 1, totalBytes - i, fitsFile);
     
     if (nr <= 0)
     {
        IDLog("Error reading temporary FITS file.\n");
        return;
     }
   }
   
   compressedBytes = sizeof(char) * totalBytes + totalBytes / 64 + 16 + 3;
     
   r = compress2(compressedData, &compressedBytes, fitsData, totalBytes, 9);
   if (r != Z_OK)
   {
 	/* this should NEVER happen */
 	IDLog("internal error - compression failed: %d\n", r);
	return;
   }
   
   snprintf(frameSize, FRAME_ILEN, "<Data type='FITS' size='%ld' compsize='%ld' />", totalBytes, compressedBytes);
   frameLen = strlen(frameSize);
   r = 0;
   
     for (i=0; i < nclients; i++)
     {
  	if (INDIClients[i].streamReady)
        {
	
	      /* #1 send frame size to client */
	     for (j=0; j < frameLen; j+=r)
	      r = write(INDIClients[i].wp, frameSize + j , frameLen - j);
	      
	     /* Send the actual file */
             for (nr=0; nr < compressedBytes; nr+=n)
   	     {
	  	n = write(INDIClients[i].wp, compressedData + nr, compressedBytes - nr );
		if (n <= 0)
		{
			if (nr <= 0)
    			{
      				IDLog("Stream error: %s\n", strerror(errno));
      				return;
    			}
		        break;
		}		
             }
        }
      } 
    
   
   free (fitsData);   
   free (compressedData);
   
}

/* Initiates the exposure procedure */
void handleExposure(void *p)
{
  long err;
  
  /* no warning */
  p=p;
  
  /* BIAS frame is the same as DARK but with minimum period. i.e. readout from camera electronics.
  */
  if (FLIImg->frameType == BIAS_FRAME)
  {
     if ((err = FLISetExposureTime(fli_dev, 50)))
     {
       ExposeTimeNP.s = IPS_IDLE;
       IDSetNumber(&ExposeTimeNP, "FLISetExposureTime() failed. %s.\n", strerror((int)-err));
       IDLog("FLISetExposureTime() failed. %s.\n", strerror((int)-err));
       return;
     }
   }
    
  if ((err = FLIExposeFrame(fli_dev)))
  {
    	ExposeTimeNP.s = IPS_IDLE;
    	IDSetNumber(&ExposeTimeNP, "FLIExposeFrame() failed. %s.", strerror((int)-err));
    	IDLog("FLIExposeFrame() failed. %s.\n", strerror((int)-err));
    	return;
  }
 
   ExposeTimeNP.s = IPS_BUSY;
		  
   IDSetNumber(&ExposeTimeNP, "Taking a %g seconds frame...", ExposeTimeN[0].value);
   
   IDLog("Taking a frame...\n");
}

/* Retrieves basic data from the CCD upon connection like temperature, array size, firmware..etc */
void getBasicData()
{

  char buff[2048];
  long err;

  IDLog("In getBasicData()\n");
  
  if ((err = FLIGetModel (fli_dev, buff, 2048)))
  {
    IDMessage(mydev, "FLIGetModel() failed. %s.", strerror((int)-err));
    IDLog("FLIGetModel() failed. %s.\n", strerror((int)-err));
    return;
  }
  else
  {
    if ( (FLICam->model = malloc (sizeof(char) * 2048)) == NULL)
    {
      IDMessage(mydev, "malloc() failed.");
      IDLog("malloc() failed.");
      return;
    }
    
    strcpy(FLICam->model, buff);
  }
  
  if (( err = FLIGetHWRevision(fli_dev, &FLICam->HWRevision)))
  {
    IDMessage(mydev, "FLIGetHWRevision() failed. %s.", strerror((int)-err));
    IDLog("FLIGetHWRevision() failed. %s.\n", strerror((int)-err));
    
    return;
  }
  
  if (( err = FLIGetFWRevision(fli_dev, &FLICam->FWRevision)))
  {
    IDMessage(mydev, "FLIGetFWRevision() failed. %s.", strerror((int)-err));
    IDLog("FLIGetFWRevision() failed. %s.\n", strerror((int)-err));
    return;
  }

  if (( err = FLIGetPixelSize(fli_dev, &FLICam->x_pixel_size, &FLICam->y_pixel_size)))
  {
    IDMessage(mydev, "FLIGetPixelSize() failed. %s.", strerror((int)-err));
    IDLog("FLIGetPixelSize() failed. %s.\n", strerror((int)-err));
    return;
  }
  
  FLICam->x_pixel_size *= 1e6;
  FLICam->y_pixel_size *= 1e6; 
  
  if (( err = FLIGetArrayArea(fli_dev, &FLICam->Array_Area[0], &FLICam->Array_Area[1], &FLICam->Array_Area[2], &FLICam->Array_Area[3])))
  {
    IDMessage(mydev, "FLIGetArrayArea() failed. %s.", strerror((int)-err));
    IDLog("FLIGetArrayArea() failed. %s.\n", strerror((int)-err));
    return;
  }
  
  if (( err = FLIGetVisibleArea( fli_dev, &FLICam->Visible_Area[0], &FLICam->Visible_Area[1], &FLICam->Visible_Area[2], &FLICam->Visible_Area[3])))
  {
    IDMessage(mydev, "FLIGetVisibleArea() failed. %s.", strerror((int)-err));
    IDLog("FLIGetVisibleArea() failed. %s.\n", strerror((int)-err));
  }
  
  if (( err = FLIGetTemperature(fli_dev, &FLICam->temperature)))
  {
    IDMessage(mydev, "FLIGetTemperature() failed. %s.", strerror((int)-err));
    IDLog("FLIGetTemperature() failed. %s.\n", strerror((int)-err));
    return;
  }
  
  IDLog("The CCD Temperature is %f.\n", FLICam->temperature);
  
  PixelSizeN[0].value  = FLICam->x_pixel_size;				/* Pixel width (um) */
  PixelSizeN[1].value  = FLICam->y_pixel_size;				/* Pixel height (um) */
  TemperatureN[0].value = FLICam->temperature;				/* CCD chip temperatre (degrees C) */
  FrameN[0].value = 0;							/* X */
  FrameN[1].value = 0;							/* Y */
  FrameN[2].value = FLICam->Visible_Area[2] - FLICam->Visible_Area[0];	/* Frame Width */
  FrameN[3].value = FLICam->Visible_Area[3] - FLICam->Visible_Area[1];	/* Frame Height */
  
  FLICam->width  = FLIImg->width = FrameN[2].value;
  FLICam->height = FLIImg->width = FrameN[3].value;
  
  BinningN[0].value = BinningN[1].value = 1;
  
  IDLog("The Camera Width is %d ---- %d\n", (int) FLICam->width, (int) FrameN[2].value);
  IDLog("The Camera Height is %d ---- %d\n", (int) FLICam->height, (int) FrameN[3].value);
  
  IDSetNumber(&PixelSizeNP, NULL);
  IDSetNumber(&TemperatureNP, NULL);
  IDSetNumber(&FrameNP, NULL);
  IDSetNumber(&BinningNP, NULL);
  
  IDLog("Exiting getBasicData()\n");
  
}

int manageDefaults(char errmsg[])
{
  long err;
  int exposeTimeMS;
  
  exposeTimeMS = (int) (ExposeTimeN[0].value * 1000.);
  
  IDLog("Setting default exposure time of %d ms.\n", exposeTimeMS);
  if ( (err = FLISetExposureTime(fli_dev, exposeTimeMS) ))
  {
    sprintf(errmsg, "FLISetExposureTime() failed. %s.\n", strerror((int)-err));
    IDLog(errmsg, NULL);
    return -1;
  }
  
  /* Default frame type is NORMAL */
  if ( (err = FLISetFrameType(fli_dev, FLI_FRAME_TYPE_NORMAL) ))
  {
    sprintf(errmsg, "FLISetFrameType() failed. %s.\n", strerror((int)-err));
    IDLog(errmsg, NULL);
    return -1;
  }
  
  /* X horizontal binning */
  if ( (err = FLISetHBin(fli_dev, BinningN[0].value) ))
  {
    sprintf(errmsg, "FLISetBin() failed. %s.\n", strerror((int)-err));
    IDLog(errmsg, NULL);
    return -1;
  }
  
  /* Y vertical binning */
  if ( (err = FLISetVBin(fli_dev, BinningN[1].value) ))
  {
    sprintf(errmsg, "FLISetVBin() failed. %s.\n", strerror((int)-err));
    IDLog(errmsg, NULL);
    return -1;
  }
  
  IDLog("Setting default binning %f x %f.\n", BinningN[0].value, BinningN[1].value);
  
  FLISetNFlushes(fli_dev, NFLUSHES);
  
  /* Set image area */
  if (setImageArea(errmsg))
    return -1;
  
  /* Success */
  return 0;
    
}

int getOnSwitch(ISwitchVectorProperty *sp)
{
  int i=0;
 for (i=0; i < sp->nsp ; i++)
 {
   /*IDLog("Switch %s is %s\n", sp->sp[i].name, sp->sp[i].s == ISS_ON ? "On" : "Off");*/
     if (sp->sp[i].s == ISS_ON)
      return i;
 }

 return -1;
}

int checkPowerS(ISwitchVectorProperty *sp)
{
  if (PowerSP.s != IPS_OK)
  {
    if (!strcmp(sp->label, ""))
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", sp->name);
    else
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", sp->label);
	
    sp->s = IPS_IDLE;
    IDSetSwitch(sp, NULL);
    return -1;
  }

  return 0;
}

int checkPowerN(INumberVectorProperty *np)
{
  if (PowerSP.s != IPS_OK)
  {
     if (!strcmp(np->label, ""))
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", np->name);
    else
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", np->label);
    
    np->s = IPS_IDLE;
    IDSetNumber(np, NULL);
    return -1;
  }

  return 0;
}

int checkPowerT(ITextVectorProperty *tp)
{

  if (PowerSP.s != IPS_OK)
  {
    if (!strcmp(tp->label, ""))
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", tp->name);
    else
    	IDMessage (mydev, "Cannot change property %s while the CCD is offline.", tp->label);
	
    tp->s = IPS_IDLE;
    IDSetText(tp, NULL);
    return -1;
  }

  return 0;

}

void connectCCD()
{
  long err;
  char errmsg[1024];
 
  IDLog ("In ConnectCCD\n");
   
  /* USB by default {USB, SERIAL, PARALLEL, INET} */
  switch (PowerS[0].s)
  {
    case ISS_ON:
      IDLog("Current portSwitch is %d\n", portSwitchIndex);
      IDLog("Attempting to find the camera in domain %ld\n", Domains[portSwitchIndex]);
      if (findcam(Domains[portSwitchIndex]))
      {
	  PowerSP.s = IPS_IDLE;
	  PowerS[0].s = ISS_OFF;
	  PowerS[1].s = ISS_ON;
	  IDSetSwitch(&PowerSP, "Error: no cameras were detected.");
	  IDLog("Error: no cameras were detected.\n");
	  return;
      }
      
      if ((err = FLIOpen(&fli_dev, FLICam->name, FLIDEVICE_CAMERA | FLICam->domain)))
      {
          PowerSP.s = IPS_IDLE;
	  PowerS[0].s = ISS_OFF;
	  PowerS[1].s = ISS_ON;
	  IDSetSwitch(&PowerSP, "Error: FLIOpen() failed. %s.", strerror( (int) -err));
	  IDLog("Error: FLIOpen() failed. %s.\n", strerror( (int) -err));
	  return;
      }
      
      /* Sucess! */
      PowerS[0].s = ISS_ON;
      PowerS[1].s = ISS_OFF;
      PowerSP.s = IPS_OK;
      IDSetSwitch(&PowerSP, "CCD is online. Retrieving basic data.");
      IDLog("CCD is online. Retrieving basic data.\n");
      getBasicData();
      if (manageDefaults(errmsg))
      {
        IDMessage(mydev, errmsg, NULL);
	IDLog(errmsg);
	return;
      }
      
      break;
      
    case ISS_OFF:
      PowerS[0].s = ISS_OFF;
      PowerS[1].s = ISS_ON;
      PowerSP.s = IPS_IDLE;
      IDSetSwitch(&PowerSP, "CCD is offline.");
      break;
     }
}

/* isCCDConnected: return 1 if we have a connection, 0 otherwise */
int isCCDConnected(void)
{
  return ((PowerS[0].s == ISS_ON) ? 1 : 0);
}

int findcam(flidomain_t domain)
{
  char **tmplist;
  long err;
  
  IDLog("In find Camera, the domain is %ld\n", domain);

  if (( err = FLIList(domain | FLIDEVICE_CAMERA, &tmplist)))
  {
    IDLog("FLIList() failed. %s\n", strerror((int)-err));
    return -1;
  }
  
  if (tmplist != NULL && tmplist[0] != NULL)
  {
    int i;

    IDLog("Trying to allocate memory to FLICam\n");
    if ((FLICam = malloc (sizeof (cam_t))) == NULL)
    {
        IDLog("malloc() failed.\n");
	return -1;
    }

    for (i = 0; tmplist[i] != NULL; i++)
    {
      int j;

      for (j = 0; tmplist[i][j] != '\0'; j++)
	if (tmplist[i][j] == ';')
	{
	  tmplist[i][j] = '\0';
	  break;
	}
    }
    
     FLICam->domain = domain;
     
      /* Each driver handles _only_ one camera for now */
      switch (domain)
      {
      case FLIDOMAIN_PARALLEL_PORT:
	strcpy(FLICam->dname, "parallel port");
	break;

      case FLIDOMAIN_USB:
	strcpy(FLICam->dname , "USB");
	break;

      case FLIDOMAIN_SERIAL:
	strcpy(FLICam->dname , "serial");
	break;

      case FLIDOMAIN_INET:
	strcpy (FLICam->dname , "inet");
	break;

      default:
	strcpy(FLICam->dname , "Unknown domain");
      }
      
      FLICam->name = strdup(tmplist[0]);
      
     if ((err = FLIFreeList(tmplist)))
     {
       IDLog("FLIFreeList() failed. %s.\n", strerror((int)-err));
       return -1;
     }
      
   } /* end if */
   else
   {
     if ((err = FLIFreeList(tmplist)))
     {
       IDLog("FLIFreeList() failed. %s.\n", strerror((int)-err));
       return -1;
     }
     
     return -1;
   }

  IDLog("Findcam() finished successfully.\n");
  return 0;
}

FITS_HDU_LIST * create_fits_header (FITS_FILE *ofp, uint width, uint height, uint bpp)
{
 FITS_HDU_LIST *hdulist;
 char temp_s[80], expose_s[80], binning_s[80], pixel_s[80], frame_s[80];
 char obsDate[80];
 char ts[32];
 struct tm *tp;
 time_t t;
 
 time (&t);
 tp = gmtime (&t);
 strftime (ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tp);
 
 snprintf(obsDate, 80, "DATE-OBS= '%s' /Observation Date UTC", ts);
 
 hdulist = fits_add_hdu (ofp);
 if (hdulist == NULL) return (NULL);

 hdulist->used.simple = 1;
 hdulist->bitpix = 16;/*sizeof(unsigned short) * 8;*/
 hdulist->naxis = 2;
 hdulist->naxisn[0] = width;
 hdulist->naxisn[1] = height;
 hdulist->naxisn[2] = bpp;
 hdulist->used.datamin = min();
 hdulist->datamin = min();
 hdulist->used.datamax = max();
 hdulist->datamax = max();
 hdulist->used.bzero = 1;
 hdulist->bzero = 0.0;
 hdulist->used.bscale = 1;
 hdulist->bscale = 1.0;
 
 sprintf(temp_s, "CCD-TEMP= %g / degrees celcius", TemperatureN[0].value);
 sprintf(expose_s, "EXPOSURE= %d / milliseconds", FLIImg->expose);
 sprintf(binning_s, "BINNING = '(%g x %g)'", BinningN[0].value, BinningN[1].value);
 sprintf(pixel_s, "PIX-SIZ = '%.0f microns square'", PixelSizeN[0].value);
 switch (FLIImg->frameType)
  {
    case LIGHT_FRAME:
      	strcpy(frame_s, "FRAME   = 'Light'");
	break;
    case BIAS_FRAME:
        strcpy(frame_s, "FRAME   = 'Bias'");
	break;
    case FLAT_FRAME:
        strcpy(frame_s, "FRAME   = 'Flat Field'");
	break;
    case DARK_FRAME:
        strcpy(frame_s, "FRAME   = 'Dark'");
	break;
  }
 
 fits_add_card (hdulist, frame_s);   
 fits_add_card (hdulist, temp_s);
 fits_add_card (hdulist, expose_s);
 fits_add_card (hdulist, pixel_s);
 fits_add_card (hdulist, "INSTRUME= 'Finger Lakes Instruments'");
 fits_add_card (hdulist, obsDate);
  
 return (hdulist);
}

double min()
{
  double lmin = FLIImg->img[0];
  int ind=0, i, j;
  
  for (i= 0; i < FLIImg->height ; i++)
    for (j= 0; j < FLIImg->width; j++)
    {
       ind = (i * FLIImg->width) + j;
       if (FLIImg->img[ind] < lmin) lmin = FLIImg->img[ind];
    }
    
    return lmin;
}

double max()
{
  double lmax = FLIImg->img[0];
  int ind=0, i, j;
  
   for (i= 0; i < FLIImg->height ; i++)
    for (j= 0; j < FLIImg->width; j++)
    {
      ind = (i * FLIImg->width) + j;
       if (FLIImg->img[ind] > lmax) lmax = FLIImg->img[ind];
    }
    
    return lmax;
}


void initDataChannel ()
{
	int pid;
	int rp[2], wp[2];

	/* new pipes */
	if (pipe (rp) < 0) {
	    fprintf (stderr, "%s: read pipe: %s\n", me, strerror(errno));
	    exit(1); 
	}
	if (pipe (wp) < 0) {
	    fprintf (stderr, "%s: write pipe: %s\n", me, strerror(errno));
	    exit(1);
	}

	/* new process */
	pid = fork();
	if (pid < 0) {
	    fprintf (stderr, "%s: fork: %s\n", me, strerror(errno));
	    exit(1);
	}
	if (pid == 0) {
	    /* child: listen to driver */
	    close(wp[1]);
	    close(rp[0]);
	    
	    waitForData(wp[0], rp[1]);
	    
	    return;
	}

	close (wp[0]);
	close (rp[1]);
	INDIClients[nclients].rp = rp[0];
	INDIClients[nclients].wp = wp[1];
	
	nclients++;
	
	fprintf(stderr, "Going to listen on fd %d, and write to fd %d\n", rp[0], wp[1]);
	updateDataChannel(NULL);

}

void updateDataChannel(void *p)
{
  char buffer[1];
  fd_set rs;
  unsigned int nr, i;
  struct timeval tv;
  tv.tv_sec  = 0;
  tv.tv_usec = 0;
  p=p;
  
  for (i=0; i < nclients; i++)
  {
        if (INDIClients[i].rp < 0)
	 continue;
	 
  	FD_ZERO(&rs);
  	FD_SET(INDIClients[i].rp, &rs);
  
  	nr = select(INDIClients[i].rp+1, &rs, NULL, NULL, &tv);
  
  	if (nr <= 0)
	 continue;

  	nr = read(INDIClients[i].rp, buffer, 1);
  	if (nr > 0 && atoi(buffer) == 0)
	{
	        IDLog("Client %d is ready to receive stream\n", i);
		INDIClients[i].streamReady = 1;
	}
	else
	{
		INDIClients[i].streamReady = 0;
		INDIClients[i].rp          = -1;
		IDLog("Lost connection with client %d\n", i);
	}
   }
  
   addTimer(1000, updateDataChannel, NULL);
  
}

int findPort()
{
  struct sockaddr_in serv_socket;
  int sfd;
  int port = 8000;
  int i=0;
  
  /* make socket endpoint */
  if ((sfd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
	    fprintf (stderr, "%s: socket: %s", me, strerror(errno));
	    exit(1);
  }
	
  /* bind to given port for any IP address */
  memset (&serv_socket, 0, sizeof(serv_socket));
  serv_socket.sin_family = AF_INET;
  serv_socket.sin_addr.s_addr = htonl (INADDR_ANY);
  
  for (i=0; i < 100; i++)
  {
  	serv_socket.sin_port = htons ((unsigned short)port);
	if (bind(sfd,(struct sockaddr*)&serv_socket,sizeof(serv_socket)) < 0)
	{
	  	fprintf (stderr, "%s: bind: %s\n", me, strerror(errno));
		port +=5;
	}
	else break;
  }
  
  close(sfd);
  if (i == 100) return -1;
  
  return port;
	
}
		
  
void waitForData(int rp, int wp)
{
        struct sockaddr_in serv_socket;
	struct sockaddr_in cli_socket;
	int cli_len;
	int sfd, lsocket;
	int reuse = 1;
        fd_set rs;
	int maxfd;
	int i, s, nr, n;
	unsigned char buffer[PIPEBUFSIZ];
	char dummy[8];
	
	/* make socket endpoint */
	if ((sfd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
	    fprintf (stderr, "%s: socket: %s", me, strerror(errno));
	    exit(1);
	}
	
	/* bind to given port for any IP address */
	memset (&serv_socket, 0, sizeof(serv_socket));
	serv_socket.sin_family = AF_INET;
	serv_socket.sin_addr.s_addr = htonl (INADDR_ANY);
	
	serv_socket.sin_port = htons ((unsigned short)DataPort);
	if (setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) < 0){
		fprintf (stderr, "%s: setsockopt: %s", me, strerror(errno));
	    	exit(1);
		}
	if (bind(sfd,(struct sockaddr*)&serv_socket,sizeof(serv_socket)) < 0){
	    	fprintf (stderr, "%s: bind: %s\n", me, strerror(errno));
		exit(1);
	    	}
	
        /* Tell client we're ready to connect */
	/* N.B. This only modifies the child DataChannelNP, the parent's property remains unchanged. */
	DataChannelN[0].value = DataPort;
        DataChannelNP.s = IPS_OK;
        IDSetNumber(&DataChannelNP, NULL);
	
	/* willing to accept connections with a backlog of 1 pending */
	if (listen (sfd, 1) < 0) {
	    fprintf (stderr, "%s: listen: %s", me, strerror(errno));
	    exit(1);
	}

	/* ok */
	lsocket = sfd;
        fprintf (stderr, "%s: listening to data port %d on fd %d\n",me,DataPort,sfd);

	/* start with public contact point */
	FD_ZERO(&rs);
	FD_SET(lsocket, &rs);
	maxfd = lsocket;

	/* wait for action */
	s = select (maxfd+1, &rs, NULL, NULL, NULL);
	if (s < 0) {
	    fprintf (stderr, "%s: select: %s\n", me, strerror(errno));
	    exit(1);
	}
 
	/* get a private connection to new client */
	cli_len = sizeof(cli_socket);
	s = accept (lsocket, (struct sockaddr *)&cli_socket, (socklen_t *)&cli_len);
	if(s < 0) {
	    fprintf (stderr, "%s: accept: %s", me, strerror(errno));
	    exit (1);
	}
	
	/* Tell the driver that this client established a connection to the data channel and is ready
	   to receive stream data */
	sprintf(dummy, "%d", 0);
	write(wp, dummy, 1);
	
	FD_ZERO(&rs);
	FD_SET(rp, &rs);
	
	while (1)
	{
		i = select(rp+1, &rs, NULL, NULL, NULL);
		if (i < 0)
		{
	  		fprintf (stderr, "%s: select: %s\n", me, strerror(errno));
			/* Tell driver the client has disconnected the data channel */
			sprintf(dummy, "%d", -1);
			write(wp, dummy, 1);
	  		exit(1);
		}
		
		nr = read(rp, buffer, PIPEBUFSIZ);
		if (nr < 0) {
		sprintf(dummy, "%d", -1);
		write(wp, dummy, 1);
	    	exit(1);
		}
		if (nr == 0) {
		sprintf(dummy, "%d", -1);
		write(wp, dummy, 1);
		fprintf (stderr, "Client %d: EOF\n", s);
	    	exit(1);
		}
		
		for (i=0; i < nr; i+=n)
		{
		  n = write(s, buffer + i, nr - i);
		  
		  if (n <= 0)
		  {
		        fprintf(stderr, "nr is %d\n", nr);
			
			if (n < 0)
		    		fprintf(stderr, "%s\n", strerror(errno));
			else
		    	        fprintf(stderr, "Short write of FITS pixels.\n");
				
		       sprintf(dummy, "%d", -1);
		       write(wp, dummy, 1);
		       exit(1);
		  }		
		}
		
	}

}
