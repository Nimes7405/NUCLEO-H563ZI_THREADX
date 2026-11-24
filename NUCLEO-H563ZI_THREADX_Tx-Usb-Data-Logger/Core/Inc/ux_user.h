#ifndef UX_USER_H
#define UX_USER_H

/* Only the device side of USBX is linked: a single CDC-ACM instance, no host
   stack, no OTG. Anything left at its default is documented in ux_port.h. */

/* The class array is sized by this value; one CDC-ACM class is enough. */
#define UX_MAX_SLAVE_CLASS_DRIVER       1U

/* CDC-ACM keeps its out/in transfers on one interface pair. */
#define UX_MAX_SLAVE_INTERFACES         2U

/* Internal USBX threads (device controller, class helpers). */
#ifndef UX_THREAD_STACK_SIZE
#define UX_THREAD_STACK_SIZE            (1024)
#endif

#endif
