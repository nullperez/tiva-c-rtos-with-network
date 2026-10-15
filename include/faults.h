/******************************************************************************
 * File:        faults.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/13/24
 *
 * Description: Fault handlers
 ******************************************************************************/

#ifndef FAULTS_H_
#define FAULTS_H_

void enableFaultHandlers(void);
void busFaultIsr(void);
void usageFaultIsr(void);
void hardFaultIsr(void);
void mpuFaultIsr(void);

#endif
