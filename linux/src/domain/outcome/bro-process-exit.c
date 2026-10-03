/* bro-process-exit.c */
#include "bro-process-exit.h"

BroProcessExit
bro_process_exit_status (int status)
{
  BroProcessExit e = { status, 0, -1, FALSE, FALSE };
  return e;
}
