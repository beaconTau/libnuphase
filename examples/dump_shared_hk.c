#include "beacon.h" 
#include <stdio.h> 
#include "zlib.h"
#include "string.h" 


int main(int nargs, char ** args) 
{

  if (nargs < 2) 
  {
    fprintf(stderr,"dump_shared_hk hk.bin"); 
    return 1; 
  }



  FILE * f = fopen(args[1], "r"); 
  beacon_hk_t hk;

  fread(&hk, sizeof(hk),1,f); 

  beacon_hk_print(stdout, &hk); 
  fclose(f); 

  return 0; 

}
