# TheoryCraftOS
A theoretically optimal operating system design for single user devices.


## Design Summary
- Monolithic Kernel Design
- Single User
- Hybrid Scheduler
- Custom Syscall interface

## Motivation & Focus
- Workstation Focus
  - Modern operating system focus has shifted from getting work done locally on workstation type devices to cloud workloads. That trend is nothing new workloads shift between centralized and decentralized periodically (terminal/mainframe, Terminal servers, Virtual Desktops, etc). However workstations share operating systems with servers whether you are using Windows or Linux. This means that trade-offs at the design level will favor server workloads.
- Single User Focus
  - User account security is a major source of frustration for even the most tech savy individuals. In truth this feature provides no protection in a single user environment because everything the user cares about (Documents, Pictures, Source Code, etc) is all accessible to a rogue program as well as a well behaved one. The only protection offered by this is preventing a user from changing another users experience on the same machine. So by focusing on a single user system we remove the complexity, lost performance, and frustration associated user account security while still providing the user with the same level of real world protection of a more mainstream operating system.
  - Another benefit of single user focus is that cooperative multi-tasking can be allowed. In a server environment a user must never be allowed to use all of a servers resources when other users having pending work. On a single user device however as long as that is the users desire a program can be allowed to use as much CPU time as it needs as long as some CPU time is reserved for any required background operations. This opens the door for creative optimizations and potentially better utilization of CPU Cache.
-Feature Rich Kernel
  - Mainstream operating systems are largely locked into existing interfaces with userland due to the need to support legacy software. That makes it challenging to explore alternatives such as the fully async implementation used by Managarm OS. With a clean design real world usage patterns can be used to craft a new kernel interface that makes good use of modern hardware with modern use cases.
