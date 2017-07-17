#define _GNU_SOURCE //required for ppoll. 

#include "nuphasedaq.h" 
#include <linux/spi/spidev.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h> 
#include <signal.h>
#include <inttypes.h>
#include <errno.h> 
#include <endian.h>

#define NP_ADDRESS_MAX 128 
#define NP_SPI_BYTES  NP_WORD_SIZE
#define NP_NUM_MODE 4
#define NP_NUM_REGISTER 128
#define BUF_MASK 0xf
#define MAX_PRETRIGGER 8 
#define BOARD_CLOCK_HZ 7500000

#define SPI_CAST  (uintptr_t) 

#define NP_DELAY_USECS 0

#define POLL_USLEEP 1000 


//register map 
typedef enum
{
  REG_SET_READ_REG=0x00, 
  REG_FIRMWARE_VER=0x01, 
  REG_FIRMWARE_DATE=0x02, 
  REG_STATUS = 0x03, 
  REG_CHIPID_LOW = 0x04,  
  REG_CHIPID_MID = 0x05,  
  REG_CHIPID_HI = 0x06,  
  REG_SCALER_READ = 0x07, 
  REG_EVENT_COUNTER_LOW = 0xa, 
  REG_EVENT_COUNTER_HIGH = 0xb, 
  REG_TRIG_COUNTER_LOW = 0xc, 
  REG_TRIG_COUNTER_HIGH = 0xd, 
  REG_TRIG_TIME_LOW = 0xe, 
  REG_TRIG_TIME_HIGH = 0xf, 
  REG_DEADTIME = 0x10, 
  REG_TRIG_INFO = 0x11,  //bits 23-22 : event buffer ; bits16-15: trig type ; bits 14-0: last beam trigger
  REG_TRIG_MASKS = 0x12,  // bits 22-15 : channel mask ; bits 14-0 : beam mask
  REG_BEAM_POWER= 0x14,   // add beam to get right register
  REG_UPDATE_SCALERS = 0x28, 
  REG_PICK_SCALER = 0x29, 
  REG_CALPULSE=0x2a, //cal pulse
  REG_CHANNEL_MASK=0x30, 
  REG_READ=0x47, //send data to spi miso 
  REG_FORCE_TRIG=0x40, 
  REG_CHANNEL=0x41, //select channel to read
  REG_MODE=0x42, //readout mode
  REG_RAM_ADDR=0x45, //ram address
  REG_CHUNK=0x49, //which 32-bit chunk 
  REG_PRETRIGGER=0x4c, 
  REG_CLEAR=0x4d, //clear buffers 
  REG_BUFFER=0x4e,
  REG_TRIGGER_MASK =0x50, 
  REG_THRESHOLDS= 0x56, // add the threshold to this to get the right register
  REG_RESET_COUNTER = 0x7e, 
  REG_RESET_ALL= 0x7f 

} nuphase_register_t; 


//readout modes 
typedef enum 
{
  MODE_REGISTER=0,
  MODE_WAVEFORMS=1,
  MODE_BEAMS=2,
  MODE_POWERSUM=3
} nuphase_readout_mode_t; 

struct nuphase_dev
{
  const char * device_name; 
  int spi_fd; 
  int gpio_fd; 
  int enable_locking; 
  uint64_t event_number_offset; 
  uint64_t event_counter;  // should match device...we'll keep this to complain if it doesn't
  nuphase_config_t cfg; 
  uint16_t buffer_length; 
  pthread_mutex_t mut; //mutex for the SPI (not for the gpio though). Only used if enable_locking is true
  pthread_mutex_t wait_mut; //mutex for the waiting. Only used if enable_locking is true
  uint8_t board_id; 
  volatile int cancel_wait; // needed for signal handlers 
  struct timespec start_time; //the time of the last clock reset
  long waiting_thread;     // needed for signal handlers (if gpio is used) 
}; 


//some macros 
#define USING(d) if (d->enable_locking) pthread_mutex_lock(&d->mut);
#define DONE(d)  if (d->enable_locking) pthread_mutex_unlock(&d->mut);

// all possible buffers we might batch
static uint8_t buf_mode[NP_NUM_MODE][NP_SPI_BYTES];
static uint8_t buf_set_read_reg[NP_NUM_REGISTER][NP_SPI_BYTES];
static uint8_t buf_channel[NP_NUM_CHAN][NP_SPI_BYTES];
static uint8_t buf_buffer[NP_NUM_BUFFER][NP_SPI_BYTES];
static uint8_t buf_chunk[NP_NUM_CHUNK][NP_SPI_BYTES];
static uint8_t buf_ram_addr[NP_ADDRESS_MAX][NP_SPI_BYTES];
static uint8_t buf_clear[1 << NP_NUM_BUFFER][NP_SPI_BYTES];
static uint8_t buf_pick_scaler[NP_NUM_BEAMS][NP_SPI_BYTES]; 

static uint8_t buf_read[NP_SPI_BYTES] = {REG_READ,0,0,0}; 
static uint8_t buf_update_scalers[NP_SPI_BYTES] = {REG_UPDATE_SCALERS,0,0,1} ; 
static uint8_t buf_reset[NP_SPI_BYTES] = {REG_RESET_ALL,0,0,1}; 
static uint8_t buf_reset_counter[NP_SPI_BYTES] = {REG_RESET_COUNTER,0,0,1}; 
static uint8_t buf_clear_all_masks[NP_SPI_BYTES] = {REG_TRIGGER_MASK,0,0,0xe}; 

static void fillBuffers() __attribute__((constructor)); //this will fill them


void fillBuffers()
{
  int i; 

  memset(buf_mode,0,sizeof(buf_mode)); 
  for (i = 0; i < NP_NUM_MODE; i++) 
  {
    buf_mode[i][0] = REG_MODE; 
    buf_mode[i][3] = i; 
  }


  memset(buf_set_read_reg,0,sizeof(buf_set_read_reg)); 
  for (i = 0; i < NP_NUM_REGISTER; i++) 
  {
    buf_set_read_reg[i][0]=  REG_SET_READ_REG; 
    buf_set_read_reg[i][3] = i; 
  }

  memset(buf_channel,0,sizeof(buf_channel)); 
  for (i = 0; i < NP_NUM_CHAN; i++) 
  {
    buf_channel[i][0] = REG_CHANNEL; 
    buf_channel[i][3] = i; 
  }

  memset(buf_buffer,0,sizeof(buf_buffer)); 
  for (i = 0; i < NP_NUM_BUFFER; i++) 
  {
    buf_buffer[i][0] = REG_BUFFER; 
    buf_buffer[i][3] = i; 
  }

  memset(buf_ram_addr,0,sizeof(buf_ram_addr)); 
  for (i = 0; i < NP_ADDRESS_MAX; i++) 
  {
    buf_ram_addr[i][0] = REG_RAM_ADDR;
    buf_ram_addr[i][3] = i;
  }

  memset(buf_chunk,0,sizeof(buf_chunk)); 

  for (i = 0; i < NP_NUM_CHUNK;i++)
  {
    buf_chunk[i][0]=REG_CHUNK; 
    buf_chunk[i][3] = i; 
  }

  memset(buf_clear,0,sizeof(buf_clear)); 
  for (i = 0; i < (1 << NP_NUM_BUFFER); i++)
  {
    buf_clear[i][0]=REG_CLEAR; 
    buf_clear[i][3]=i;  
  }

  memset(buf_pick_scaler,0,sizeof(buf_pick_scaler)); 
  for (i = 0; i < NP_NUM_BEAMS; i++)
  {
    buf_pick_scaler[i][0]=REG_PICK_SCALER; 
    buf_pick_scaler[i][3]=i;  
  }
}



static void init_xfers(int n, struct spi_ioc_transfer * xfers)
{
  int i; 

  memset(xfers,0,n *sizeof(struct spi_ioc_transfer)); 
  for (i = 0; i < n; i++)
  {
    xfers[i].len = NP_SPI_BYTES; 
    xfers[i].cs_change =1; //deactivate cs between transfers
    xfers[i].delay_usecs = NP_DELAY_USECS; //? 
  }
}




static int setup_change_mode(struct spi_ioc_transfer * xfer, nuphase_readout_mode_t mode)
{
  xfer->tx_buf = SPI_CAST  buf_mode[mode]; 
  return 0; 
}



/* this will use up 3 xfer's, assumed to be zeroed already. DOES NOT SET MODE.  */
static int setup_read_register(struct spi_ioc_transfer * xfers, uint8_t address, uint8_t *result)
{

  xfers[0].tx_buf = SPI_CAST  buf_set_read_reg[address]; 
  xfers[1].tx_buf = SPI_CAST  buf_read; 
  xfers[2].rx_buf = SPI_CAST  result; 

  return 0; 
}

/* to simplify batching complicated transfers,
 * we use this simple data structure. 
 *
 */ 
struct xfer_buffer 
{
  struct spi_ioc_transfer spi[511]; 
  int nused; 
  int fd; 
};

static void xfer_buffer_init(struct xfer_buffer * b, int fd) 
{
  init_xfers(511,b->spi); 
  b->nused = 0; 
  b->fd = fd; 
}


static int xfer_buffer_send(struct xfer_buffer *b)
{
  if (!b->nused) return 0; 
  int success = ioctl(b->fd, SPI_IOC_MESSAGE(b->nused), b->spi); 
  if (!success) 
  {
    fprintf(stderr,"IOCTL failed!\n"); 
    return -1; 
  }
  b->nused = 0; 

  return 0; 
}



// this will send if full!  
static int xfer_buffer_append(struct xfer_buffer * b, const uint8_t * txbuf, const uint8_t * rxbuf) 
{
  //check if full 
  if (b->nused >= 511) //greater than just in case, but it already means something went horribly wrong 
  {
    if (xfer_buffer_send(b))
    {
      return -1; 
    }
  }

  b->spi[b->nused].tx_buf = SPI_CAST txbuf; 
  b->spi[b->nused].rx_buf = SPI_CAST rxbuf; 
  b->nused++; 
  return 0; 
}

static int xfer_buffer_read_register(struct xfer_buffer * b, uint8_t address, uint8_t * result)
{
  int ret = 0; 
  ret += xfer_buffer_append(b, buf_set_read_reg[address],0);
  ret += xfer_buffer_append(b, buf_read,0);
  ret += xfer_buffer_append(b,0,result); 
  return ret; 
}

/* this will use up 13*naddr xfers.  MUST start on chunk 0 */ 
static int loop_over_chunks_half_duplex(struct xfer_buffer * xfers, uint8_t naddr, uint8_t start_address, uint8_t * result) 
{

  int iaddr; 
  int ichunk; 
  int ret = 0; 

  for (iaddr  =0; iaddr < naddr; iaddr++) 
  {
    ret += xfer_buffer_append(xfers, buf_ram_addr[start_address + iaddr], 0); 
    if (ret) return ret; 

    for (ichunk = 0; ichunk < NP_NUM_CHUNK; ichunk++)
    {
      ret+= xfer_buffer_append(xfers, buf_chunk[ichunk], 0); 
      if (ret) return ret; 

      ret+= xfer_buffer_append(xfers, buf_read, 0); 
      if (ret) return ret; 

      ret+= xfer_buffer_append(xfers, 0, result + NP_NUM_CHUNK * iaddr + ichunk * NP_SPI_BYTES); 
      if (ret) return ret; 
    }
  }

  return 0; 
}

int nuphase_read_raw(nuphase_dev_t *d, uint8_t buffer, uint8_t channel, uint8_t start, uint8_t finish, uint8_t * data) 
{

  struct xfer_buffer xfers; 
  xfer_buffer_init(&xfers, d->spi_fd); 
  uint8_t naddress = finish - start + 1; 
  int ret = 0; 
  ret += xfer_buffer_append(&xfers, buf_mode[MODE_WAVEFORMS], 0);  if (ret) return 0; 
  ret += xfer_buffer_append(&xfers, buf_buffer[buffer], 0);  if (ret) return 0; 
  ret += xfer_buffer_append(&xfers, buf_channel[channel], 0);  if (ret) return 0; 
  USING(d);  //have to lock for the duration otherwise channel /read mode may be changed form underneath us.  
            // we don't lock before these because there is no way we sent enough transfers to trigger a read 
  ret += loop_over_chunks_half_duplex(&xfers, naddress, start, data);
  if(!ret) xfer_buffer_send(&xfers); //pick up the stragglers. 
  DONE(d);  

  return 0; 
}



int nuphase_read_register(nuphase_dev_t * d, uint8_t address, uint8_t *result)
{

  struct spi_ioc_transfer xfer[4]; 
  int ret;
  if (address > NP_ADDRESS_MAX) return -1; 
  init_xfers(4,xfer); 
  setup_change_mode(xfer, MODE_REGISTER); 
  setup_read_register(xfer+1, address,result); 
  USING(d); 
  ret = ioctl(d->spi_fd, SPI_IOC_MESSAGE(4), xfer); 
  DONE(d);
  return ret; 
}


int nuphase_sw_trigger(nuphase_dev_t * d ) 
{
  uint8_t buf[4] = { REG_FORCE_TRIG, 0,0, 1};  
  int ret; 
  USING(d); 
  ret = write(d->spi_fd, buf, NP_SPI_BYTES); 
  DONE(d); 
  return ret == NP_SPI_BYTES ? 0 : -1;  
}

int nuphase_calpulse(nuphase_dev_t * d, unsigned state) 
{
  uint8_t buf[4] = { REG_CALPULSE, 0,0, state & 0xff };  
  int ret;
  USING(d); 
  ret = write(d->spi_fd, buf, NP_SPI_BYTES); 
  DONE(d); 
  return ret == NP_SPI_BYTES ? 0 : 1 ;
}


static int board_id_counter =1; 

nuphase_dev_t * nuphase_open(const char * devicename, const char * gpio,
                             const nuphase_config_t * c,  int locking)
{
  int locked,fd; 
  nuphase_dev_t * dev; 


  fd = open(devicename, O_RDWR); 
  if (fd < 0) 
  {
    fprintf(stderr,"Could not open %s\n", devicename); 
    return 0; 
  }

  locked = flock(fd,LOCK_EX | LOCK_NB); 
  if (locked < 0) 
  {
    fprintf(stderr,"Could not get exclusive access to %s\n", devicename); 
    return 0; 
  }

  dev = malloc(sizeof(nuphase_dev_t)); 
  dev->device_name = devicename; 
  dev->spi_fd =fd;
  dev->cancel_wait = 0; 
  dev->event_counter = 0; 

  if (gpio) 
  {
    dev->gpio_fd = open(gpio, O_RDWR); 
    if (dev->gpio_fd < 0) dev->gpio_fd = 0; 
    else
    {
      uint32_t unmask = 1; //unmask the interrupt 
      write(dev->gpio_fd,&unmask,sizeof(unmask)); 
    }
  }
  else
  {
    dev->gpio_fd = 0; 
  }


  //Configure the SPI protocol 
  //TODO: need some checks here. 
  uint32_t speed = 10000000; //10 MHz 
  uint8_t mode = SPI_MODE_0; 
  ioctl(dev->spi_fd, SPI_IOC_WR_MODE, &mode); 
  ioctl(dev->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed); 


  //configuration 
  if (c) dev->cfg = *c; 
  else nuphase_config_init(&dev->cfg); 

  // if this is still running in 20 years, someone will have to fix the y2k38 problem 
  dev->event_number_offset = ((uint64_t)time(0)) << 32; 
  dev->buffer_length = 624; 
  dev->board_id = board_id_counter++; 

  dev->enable_locking = locking; 

  if (locking) 
  {
    pthread_mutex_init(&dev->mut,0); 
    pthread_mutex_init(&dev->wait_mut,0); 
  }

  if (nuphase_reset(dev, &dev->cfg,0) ); 
  {
    fprintf(stderr,"Unable to reset device... "); 
    nuphase_close(dev); 
    return 0; 
  }

  return dev; 
}

void nuphase_set_board_id(nuphase_dev_t * d, uint8_t id)
{
  if (id <= board_id_counter) board_id_counter = id+1; 
  d->board_id = id; 
}

uint8_t nuphase_get_board_id(const nuphase_dev_t * d) 
{
  return d->board_id; 
}

void nuphase_set_event_number_offset(nuphase_dev_t * d, uint64_t offset) 
{
  d->event_number_offset = offset; 
}


void nuphase_set_buffer_length(nuphase_dev_t * d, uint16_t length)
{
  d->buffer_length = length; 
}

uint16_t nuphase_get_bufferlength(const nuphase_dev_t * d) 
{
  return d->buffer_length; 
}


int nuphase_fwinfo(nuphase_dev_t * d, nuphase_fwinfo_t * info)
{

  //we need to read 5 registers, so 15 xfers 
  struct spi_ioc_transfer xfers[16]; 
  int ret; 

  uint8_t dna_low[4]; 
  uint8_t dna_mid[4]; 
  uint8_t dna_hi[4]; 

  init_xfers(16,xfers); 
  setup_change_mode(xfers, MODE_REGISTER); 
  setup_read_register(xfers+1, REG_FIRMWARE_VER, (uint8_t*) &(info->ver)); 
  setup_read_register(xfers+4, REG_FIRMWARE_DATE, (uint8_t*) &(info->date)); 
  setup_read_register(xfers+7, REG_CHIPID_LOW, dna_low); 
  setup_read_register(xfers+10, REG_CHIPID_MID, dna_mid); 
  setup_read_register(xfers+13, REG_CHIPID_HI, dna_hi); 


  USING(d); 
  ret = ioctl(d->spi_fd, SPI_IOC_MESSAGE(16), xfers); 
  DONE(d); 


  //TODO check this logic. not sure endianness is correct
  uint64_t dna_low_big =  dna_low[0] | dna_low[1] << 8 || dna_low[2] << 8;
  uint64_t dna_mid_big =  dna_mid[0] | dna_mid[1] << 8 || dna_mid[2] << 8;
  uint64_t dna_hi_big =  dna_hi[0] | dna_hi[1] << 8 ;
  info->dna =  (dna_low_big & 0xffffff) | ( (dna_mid_big & 0xffffff) << 24) | ( (dna_hi_big & 0xffff) << 48); 

  //ver and date are going to be the wrong endianness, I think 
  info->ver = be32toh(info->ver); 
  info->date = be32toh(info->date); 

  return ret; 
}


int nuphase_close(nuphase_dev_t * d) 
{
  int ret = 0; 
  nuphase_cancel_wait(d); 
  USING(d); 
  if (d->enable_locking)
  {
    //this should be allowed? 
    pthread_mutex_unlock(&d->mut); 
    ret += 64* pthread_mutex_destroy(&d->mut); 

    if (pthread_mutex_trylock(&d->wait_mut)) // lock is beind held by a thread
    {
      nuphase_cancel_wait(d); 
      pthread_mutex_lock(&d->wait_mut); 
    }

    pthread_mutex_unlock(&d->wait_mut); 
    ret += 128* pthread_mutex_destroy(&d->wait_mut); 

    d->enable_locking = 0; 
  }

  ret += flock(d->spi_fd, LOCK_UN); 
  ret += 4*close(d->spi_fd); 
  if (d->gpio_fd)
  {
    ret += 8 * close(d->gpio_fd); 
  }
  free(d); 
  return ret; 
}

void nuphase_cancel_wait(nuphase_dev_t *d) 
{
  d->cancel_wait = 1;  //this is necessary for polling and will sometimes catch the interrupt as well
  if (d->waiting_thread) 
  {
    syscall(SYS_tgkill, getpid(), d->waiting_thread, SIGINT);  //we need to send a SIGINT in case we are stuck inside poll. 
  }
}

// this is the most annoying (and probably buggiest) part of the code.
// it's difficult because we want to be  able to quickly cancel a 
//
int nuphase_wait(nuphase_dev_t * d, nuphase_buffer_mask_t * ready_buffers, float timeout) 
{

  //If locking is enabled and a second thread attempts
  //to wait for the same device, return EBUSY. 
  // making nuphase_wait for multiple threads sounds way too hard
  if (d->enable_locking) 
  {
    if (pthread_mutex_trylock(&d->wait_mut))
    {
      return EBUSY; 
    }
  }

  /* This was cancelled before (or almost concurrently with when)  we started.
  /  We'll just clear the cancel and return EAGAIN. I thought long and hard
  /  about what to do in the case that nuphase_cancel_wait is called and there
  /  is nothing waiting, but I think attempting to detect if nuphase_wait is
  /  currently called is fraught with race conditions. Anyway,
  /  nuphase_cancel_wait will probably not be called willy-nilly... usually
  /  just when you want to exit the program I imagine. 
  */
  
  if (d->cancel_wait) 
  {
    d->cancel_wait = 0; 
    return EAGAIN; 
  }



  //No gpio, we will just keep polling buffer mask
  // This is the simpler (but less efficient) case  
  if (!d->gpio_fd) 
  {
    nuphase_buffer_mask_t something = 0; 
    float waited = 0; 

    // keep trying until we either get something, are cancelled, or exceed our timeout (if we have a timeout) 
    while(!something && (timeout <= 0 || waited < timeout))
    {
      if (d->cancel_wait) break; 
      usleep(POLL_USLEEP); 
      waited += POLL_USLEEP * 1e-6; //us to s 
      something = nuphase_check_buffers(d); 
    }
    int interrupted = d->cancel_wait; //were we interrupted? 

    if (ready_buffers) *ready_buffers = something;  //save to ready
    d->cancel_wait = 0;  //clear the wait
    if (d->enable_locking) pthread_mutex_unlock(&d->wait_mut);   //unlock the mutex
    return interrupted ? EINTR : 0; 
  }

  /*
  * If we are using the gpio interrupt, this becomes more complicated.  We
  * would block on the interrupt until we exceed our timeout (which may be
  * eternity). A signal would interrupt the call, but we may be called in a
  * context where the signal may not be available , or we might be in a thread
  * where the signal will not get deliver. 
  *
  * Hopefully what I'm doing makes sense. 
  */ 

  int ret = 0;


  // tell t he device which thread is waiting
  d->waiting_thread = syscall(SYS_gettid); 

  //we want to block all signals until 
  //we get to poll. Fewer things to worry about .. 
  sigset_t old;  //save the old signal mask to restore at the end
  sigset_t all; 
  sigfillset(&all); 
  pthread_sigmask(SIG_BLOCK, &all,&old); 

  //before we poll, check again to make sure we weren't already cancelled 
  if (d->cancel_wait)
  {
    ret = EINTR;
    goto cleanup; 
  }



  // tell ppoll to listen for SIGINT. Maybe this isn't the best signal, we can change it later. 
  sigset_t pollsigs; 
  sigfillset(&pollsigs); 
  sigdelset(&pollsigs,SIGINT); 

  //set up what we need for ppoll
  struct timespec ts; 
  ts.tv_sec = (int) timeout; 
  ts.tv_nsec = (timeout - ts.tv_sec) * 1e9; 
  struct pollfd fds = { .fd = d->gpio_fd, .events = POLLIN }; 

  //call ppoll. It will tell us if we got an interrupt, or timeout, or get interrupted (hopefully) by SIGINT 
  ret = ppoll(&fds,1, timeout <=0 ? 0 : &ts ,&pollsigs);  

  if (ret == 0)  // we timed out. 
  {
    if (*ready_buffers) *ready_buffers = 0;
    goto cleanup; 
  }

  if (ret < 0)
  {
    //this means we got interrupted, I think. 
    ret = errno; 
    goto cleanup; 
  }


  //alright, now we have to read the interrupt
  uint32_t info; 
  int nb = read(d->gpio_fd,&info, sizeof(info)); 

  //if we read it successfully, we'll unmask it.
  if (nb  == sizeof(info)) 
  {
    //let's unmask the interrupt
    uint32_t unmask = 1;
    nb = write(d->gpio_fd,&unmask,sizeof(unmask));
    if (nb < 0) 
    {
      ret = errno;
      fprintf(stderr,"Couldn't unmask interrupt, and I'm going to leave it in a bad state :( \n"); 
      goto cleanup; 
    }
  }
  else
  {
    ret = errno; 
    fprintf(stderr,"Couldn't read from interrupt, and I'm going to leave it in a bad sate :( \n"); 
    goto cleanup; 
  }

 //if we made it this far, this means we unmasked the interrupt, we probably got something, 
 if (ready_buffers) *ready_buffers = nuphase_check_buffers(d); 
 ret = 0;  //we want to return success

 cleanup: 
 //we had an oops, so we haven't filled this yet. fill it with 0.
 if (ret && ready_buffers) *ready_buffers = 0; 

 //restore signal mask 
 pthread_sigmask(SIG_SETMASK, &old,0); 

 //we aren't waiting any more
 d->waiting_thread = 0; 
 //clear the wait
 d->cancel_wait = 0; 

 //unlock mutex
 if (d->enable_locking) pthread_mutex_unlock(&d->wait_mut); 

 return ret;

}



nuphase_buffer_mask_t nuphase_check_buffers(nuphase_dev_t * d) 
{

  uint8_t result; 
  nuphase_buffer_mask_t mask; 
  nuphase_read_register(d, REG_STATUS, &result); 
  mask  = result &  BUF_MASK; // only keep lower 4 bits. who knows what's in the other bits? 
  return mask; 
}

/* This just sets defaults */ 
void nuphase_config_init(nuphase_config_t * c) 
{
  int i;
  c->channel_mask = 0xff; 
  c->pretrigger = 1; //?? 
  c->trigger_mask = 0xfff; 

  for (i = 0; i < NP_NUM_BEAMS; i++)
    c->trigger_thresholds[i] = 0xfffff;  //TODO make this more sensible by default 
}


int nuphase_configure(nuphase_dev_t * d, const nuphase_config_t *c, int force ) 
{
  int written; 
  int ret = 0; 
  
  USING(d); 
  if (force || c->pretrigger != d->cfg.pretrigger)
  {
    uint8_t pretrigger_buf[] = { REG_PRETRIGGER, 0, 0, c->pretrigger}; 
    written = write(d->spi_fd, pretrigger_buf, NP_SPI_BYTES); 
    if (written == NP_SPI_BYTES) 
    {
      d->cfg.pretrigger = c->pretrigger; 
    }
    else
    {
      ret = -1; //don't try to continue if we fail . 
      goto configure_end; 
    }
  }

  if (force || c->channel_mask!= d->cfg.channel_mask)
  {
    uint8_t channel_mask_buf[]= { REG_PRETRIGGER, 0, 0, c->channel_mask}; 
    written = write(d->spi_fd, channel_mask_buf, NP_SPI_BYTES); 
    if (written == NP_SPI_BYTES) 
    {
      d->cfg.channel_mask = c->channel_mask; 
    }
    else
    {
      ret = -1; 
      goto configure_end; 
    }
  }

  if (force || c->trigger_mask != d->cfg.trigger_mask)
  {
    //TODO check the byte order
    uint8_t trigger_mask_buf[]= { REG_TRIGGER_MASK, 0, (c->trigger_mask >> 8) & 0xff, c->trigger_mask & 0xff}; 
    written = write(d->spi_fd, trigger_mask_buf, NP_SPI_BYTES); 
    if (written == NP_SPI_BYTES) 
    {
      d->cfg.trigger_mask = c->trigger_mask; 
    }
    else
    {
      ret = -1; 
      goto configure_end; 
    }
  }

  if (force || memcmp(c->trigger_thresholds, d->cfg.trigger_thresholds, sizeof(c->trigger_thresholds)))
  {
    uint8_t thresholds_buf[NP_NUM_BEAMS][NP_SPI_BYTES]; 
    struct spi_ioc_transfer xfer[NP_NUM_BEAMS]; 
    init_xfers(NP_NUM_BEAMS, xfer); 
    int i; 
    for (i = 0; i < NP_NUM_BEAMS; i++)
    {
      thresholds_buf[i][0]= REG_THRESHOLDS+i ;
      thresholds_buf[i][1]= (c->trigger_thresholds[i] >> 16 ) & 0xf;
      thresholds_buf[i][2]= ( c->trigger_thresholds[i] >> 8) & 0xff; 
      thresholds_buf[i][3]= c->trigger_thresholds[i] & 0xff;
      xfer[i].tx_buf =  SPI_CAST &thresholds_buf[i][0]; 
    }
    if(!ioctl(d->spi_fd, SPI_IOC_MESSAGE(NP_NUM_BEAMS), xfer))
    {
      //success! 
      memcpy(d->cfg.trigger_thresholds, c->trigger_thresholds, sizeof(c->trigger_thresholds)); 
    }
    else 
    {
      ret =-1; 
      goto configure_end; 
    }
  }

  configure_end: 

  DONE(d); 
  return ret; 
}



//indirection! 
int nuphase_wait_for_and_read_multiple_events(nuphase_dev_t * d, 
                                      nuphase_header_t (*headers)[NP_NUM_BUFFER], 
                                      nuphase_event_t  (*events)[NP_NUM_BUFFER])  
{
  nuphase_buffer_mask_t mask; ; 
  if (!nuphase_wait(d,&mask,-1) && mask) 
  {
    int ret; 
    ret = nuphase_read_multiple_array(d,mask,&(*headers)[0], &(*events)[0]); 
    if (!ret) return __builtin_popcount(mask); 
    else return -1; 
  }
  return 0; 
}

//yay more indirection!
int nuphase_read_single(nuphase_dev_t *d, uint8_t buffer, nuphase_header_t * header, nuphase_event_t * event)
{
  nuphase_buffer_mask_t mask = 1 << buffer; 
  return nuphase_read_multiple_ptr(d,mask,&header, &event); 

}

//woohoo, even more indirection. 
int nuphase_read_multiple_array(nuphase_dev_t *d, nuphase_buffer_mask_t mask, nuphase_header_t * headers,  nuphase_event_t * events) 
{
  nuphase_event_t * ev_ptr_array[NP_NUM_BUFFER]; 
  nuphase_header_t * hd_ptr_array[NP_NUM_BUFFER]; 
  int i; 

  for ( i = 0; i < __builtin_popcount(mask); i++)
  {
    ev_ptr_array[i] = &events[i]; 
    hd_ptr_array[i] = &headers[i]; 
  }

  return nuphase_read_multiple_ptr(d,mask,hd_ptr_array, ev_ptr_array); 
}


/* for endianness */ 
typedef union bignum 
{
      uint64_t u64; 
      uint8_t u32[2]; 
} bignum_t; 


//lazy error checking macro 
#define CHK(X) if (X) { ret++; goto the_end; } 




int nuphase_read_multiple_ptr(nuphase_dev_t * d, nuphase_buffer_mask_t mask, nuphase_header_t ** hd, nuphase_event_t ** ev)
{
  int ibuf,ichan,ibeam;
  int iout = 0; 
  int ret = 0; 
  struct xfer_buffer xfers; 
  struct timespec now; 
  xfer_buffer_init(&xfers, d->spi_fd); 

  // we need to store some stuff in an intermediate format 
  // prior to putting into the header since the bits don't match 
  bignum_t event_counter; 
  bignum_t trig_counter; 
  bignum_t trig_time; 
  uint32_t deadtime; 
  uint32_t tmask; 
  uint32_t tinfo; 


  for (ibuf = 0; ibuf < NP_NUM_BUFFER; ibuf++)
  {
    //we are not reading this event right now
    if ( (mask & (1 << ibuf)) == 0)
      continue; 

    clock_gettime(CLOCK_REALTIME, &now); 

    //grab the metadata 
    //set the buffer 
    USING(d); 
    CHK(xfer_buffer_append(&xfers, buf_buffer[ibuf],0)) 

    /**Grab metadata! */ 
    //switch to register mode  
    CHK(xfer_buffer_append(&xfers, buf_mode[MODE_REGISTER],0)) 

    //we will pretend like we are bigendian so we can just call be64toh on the u65
    CHK(xfer_buffer_read_register(&xfers,REG_EVENT_COUNTER_LOW, &event_counter.u32[1])) 
    CHK(xfer_buffer_read_register(&xfers,REG_EVENT_COUNTER_HIGH, &event_counter.u32[0])) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_COUNTER_LOW, &trig_counter.u32[1])) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_COUNTER_HIGH, &trig_counter.u32[0])) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_TIME_LOW, &trig_time.u32[1])) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_TIME_HIGH, &trig_time.u32[0])) 
    CHK(xfer_buffer_read_register(&xfers,REG_DEADTIME, (uint8_t*) &deadtime)) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_INFO, (uint8_t*) &tinfo)) 
    CHK(xfer_buffer_read_register(&xfers,REG_TRIG_MASKS,(uint8_t*) &tmask)) 
    for(ibeam = 0; ibeam < NP_NUM_BEAMS; ibeam++)
    {
      CHK(xfer_buffer_read_register(&xfers, REG_BEAM_POWER+ibeam, (uint8_t*)  &hd[iout]->beam_power[ibeam]))
    }
    CHK(xfer_buffer_send(&xfers)); //flush the metadata 
    
    // check the event counter
    event_counter.u64 = be64toh(event_counter.u64); 


    if (d->event_counter != event_counter.u64) 
    {
      fprintf(stderr,"Event counter mismatch!!! (sw: %"PRIu64", hw: %"PRIu64")\n", d->event_counter, event_counter.u64); 
    }

    //now fill in header data 
    tinfo = be32toh(tinfo); 
    tmask = be32toh(tmask); 

    uint8_t hwbuf =  (tinfo >> 22) & 0x3; 
    if ( hwbuf  != ibuf)
    {
      fprintf(stderr,"Buffer number mismatch!!! (sw: %u, hw: %u)\n", ibuf, hwbuf ); 
    }
    
    hd[iout]->event_number = d->event_number_offset + event_counter.u64; 
    hd[iout]->trig_number = be64toh(trig_counter.u64); 
    hd[iout]->buffer_length = d->buffer_length; 
    hd[iout]->pretrigger_samples = d->cfg.pretrigger* 8 * 16; //TODO define these constants somewhere
    hd[iout]->readout_time = now.tv_sec; 
    hd[iout]->readout_time_ns = now.tv_nsec; 
    hd[iout]->trig_time = be64toh(trig_time.u64); 

    hd[iout]->approx_trigger_time= d->start_time.tv_sec + trig_time.u64 / BOARD_CLOCK_HZ; 
    hd[iout]->approx_trigger_time_nsecs = d->start_time.tv_nsec + (trig_time.u64 % BOARD_CLOCK_HZ) *(1.e9 / BOARD_CLOCK_HZ); 
    if (hd[iout]->approx_trigger_time_nsecs > 1e9) 
    {
      hd[iout]->approx_trigger_time++; 
      hd[iout]->approx_trigger_time_nsecs-=1e9; 
    }

    hd[iout]->triggered_beams = tinfo & 0x7fff; 
    hd[iout]->beam_mask = tmask & 0x7fff;  
    for (ibeam = 0; ibeam < NP_NUM_BEAMS; ibeam++)
    {
      hd[iout]->beam_power[ibeam] = be32toh(hd[iout]->beam_power[ibeam]); 

    }
    hd[iout]->trig_type = (tinfo >> 15) & 0x3; 
    hd[iout]->deadtime = be32toh(deadtime); 
    hd[iout]->buffer_number = hwbuf; 
    hd[iout]->channel_mask = (tmask >> 15) & 0xff; 
    hd[iout]->channel_overflow = 0; //TODO not implemented yet 
    hd[iout]->buffer_mask = mask; //this is the current buffer mask
    hd[iout]->board_id = d->board_id; 


    d->event_counter++; 


    //and some event data 
    ev[iout]->buffer_length = d->buffer_length; 
    ev[iout]->board_id = d->board_id; 


    //now start to read the data 
    //switch to waveform mode
    CHK(xfer_buffer_append(&xfers, buf_mode[MODE_WAVEFORMS],0))

    for (ichan = 0; ichan < NP_NUM_CHAN; ichan++)
    {
      if (hd[iout]->channel_mask & (1 << ichan)) //TODO is this backwards?!??? 
      {
        CHK(xfer_buffer_append(&xfers, buf_channel[ichan],0)) 
        CHK(loop_over_chunks_half_duplex(&xfers, d->buffer_length / (NP_SPI_BYTES * NP_NUM_CHUNK),0, ev[iout]->data[ichan]))
      }
      else
      {
        memset(ev[iout]->data[ichan], 0 , hd[iout]->buffer_length); 
      }
    }
    CHK(xfer_buffer_append(&xfers, buf_clear[1 << ibuf],0))
    CHK(xfer_buffer_send(&xfers)) //flush so we can clear the buffer immediately 

    iout++; 
    DONE(d); //give othe rthings a chance to use the lock 
  }


  the_end:
  //TODO add some printout here in case of falure/ 

  return ret; 
}


int nuphase_clear_buffer(nuphase_dev_t *d, nuphase_buffer_mask_t mask) 
{
  int ret; 
  USING(d); 
  ret = write(d->spi_fd, buf_clear[mask & BUF_MASK], NP_SPI_BYTES); //TODO check if this is really the right interface 
  DONE(d); 
  return ret == NP_SPI_BYTES ? 0 : -1; 
}

int nuphase_write(nuphase_dev_t *d, const uint8_t* buffer)
{
  int written = 0; 
  USING(d); 
  written = write(d->spi_fd, buffer,NP_SPI_BYTES); 
  DONE(d); 
  return written == NP_SPI_BYTES ? 0 : -1; 
}

int nuphase_read(nuphase_dev_t *d,uint8_t* buffer)
{
  int got = 0; 
  USING(d); 
  got = read(d->spi_fd, buffer,NP_SPI_BYTES); 
  DONE(d); 
  return got == NP_SPI_BYTES ? 0 : -1; 
}



int nuphase_read_status(nuphase_dev_t *d, nuphase_status_t * st) 
{
  //TODO: fill in deadtime when I figure out how. 
  int ret; 
  int ixfer = 0; 
  int i; 
  struct timespec now; 
  uint8_t wide_scalers[NP_NUM_BEAMS][NP_SPI_BYTES]; 

  st->board_id = d->board_id; 

  //cheating  because const int is apparently not constant in C
  enum {nxfers = 1 + 1 + NP_NUM_BEAMS * 3};   //one set mode register, one update scalers, num_beams *  ( pick scaler, read register, read) 

  _Static_assert (nxfers < 512, "TOO MANY IOC MESSAGES" ); 

  struct spi_ioc_transfer xfers[nxfers];
  init_xfers(nxfers, xfers); 

  xfers[ixfer++].tx_buf = SPI_CAST buf_mode[MODE_REGISTER]; 
  xfers[ixfer++].tx_buf = SPI_CAST buf_update_scalers; 

  for (i = 0; i < NP_NUM_BEAMS; i++) 
  {
    xfers[ixfer++].tx_buf = SPI_CAST buf_pick_scaler[i]; 
    xfers[ixfer++].tx_buf = SPI_CAST buf_set_read_reg[REG_SCALER_READ]; 
    xfers[ixfer++].rx_buf = SPI_CAST wide_scalers[i]; 
  }

  clock_gettime(CLOCK_REALTIME, &now); 
  USING(d); 
  ret = ioctl(d->spi_fd, SPI_IOC_MESSAGE(nxfers), xfers); 
  DONE(d); 

  if (!ret) return ret; 

  st->deadtime = 0; //TODO 
  for (i = 0; i < NP_NUM_BEAMS; i++) 
  {
    st->scalers[i] = wide_scalers[i][3] | (wide_scalers[i][2] << 8 ); 
  }
  st->readout_time = now.tv_sec; 
  st->readout_time_ns = now.tv_nsec; 

  return 0; 
}

//todo there is probably a simpler way to calculate this... 
static struct timespec avg_time(struct timespec A, struct timespec B)
{
  struct timespec avg; 
  avg.tv_nsec = (A.tv_nsec + B.tv_nsec) /2; 
  uint32_t tmp_sum = A.tv_sec + B.tv_sec; 
  avg.tv_sec = tmp_sum/2; 
  if (tmp_sum % 2 == 1 ) 
  {
    avg.tv_nsec += 5e8; 
  }

  if (avg.tv_nsec > 1e9) 
  {
    avg.tv_sec++; 
    avg.tv_nsec-=1e9; 
  }

  return avg; 
}


int nuphase_reset(nuphase_dev_t * d,const  nuphase_config_t * c, int hard_reset)
{
  
  int wrote; 
  if (hard_reset) 
  {
    wrote = write(d->spi_fd, buf_reset, NP_ADDRESS_MAX); 
    if (wrote != NP_SPI_BYTES) 
    {
      return 1;
    }
    fprintf(stderr,"Full reset...\n"); 
    //we need to sleep for a while. how about 20 seconds? 
    sleep(20); 
    fprintf(stderr,"...done\n"); 
  }
  else
  {

    // our reset sequence will be: 
    //
    //   1) clear the beam masks to allow no triggers
    //   2) clear the buffers
    //   3) reset the counters (and try to estimate the time that we did that) 
    //   ??? 
    //   r) write the configuration 
    //
    // I don't think we need to bother doing this quickly since this will happen very rarely.

    //start by clearing the masks 
    wrote = write( d->spi_fd, buf_clear_all_masks,NP_SPI_BYTES);

    if (wrote != NP_SPI_BYTES) 
    {
      fprintf(stderr, "Unable to clear masks. Aborting reset\n"); 
      return 1; 
    }

    //clear all buffers
    wrote = write  (d->spi_fd, buf_clear[0xf], NP_SPI_BYTES); 

    if (wrote != NP_SPI_BYTES) 
    {
      fprintf(stderr, "Unable to clear buffers. Aborting reset\n"); 
      return 1; 
    }


    //then reset the counters, measuring the time before and after 
   
    struct timespec tbefore; 
    struct timespec tafter; 
    clock_gettime(CLOCK_REALTIME,&tbefore); 
    wrote = write(d->spi_fd, buf_reset_counter, NP_SPI_BYTES); 
    clock_gettime(CLOCK_REALTIME,&tafter); 

    
    

    if (wrote != NP_SPI_BYTES) 
    {
      fprintf(stderr, "Unable to reset counters. Aborting reset\n"); 
      return 1; 
    }

    //take average for the start time
    d->start_time = avg_time(tbefore,tafter); 



    //TODO any calibration necessary here?

  }
  return nuphase_configure(d,c,1); 
}

