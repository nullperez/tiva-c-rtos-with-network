#ifndef SHELL_H_
#define SHELL_H_

#define MAX_CHARS 80

// Shell task: RTOS commands run here, network commands are passed to netTask
void shell(void);
void shellRxCallback(void);

#endif
