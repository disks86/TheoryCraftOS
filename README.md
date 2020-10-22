# TheoryCraftOS
A theoretically optimal operating system design for single user devices.


## Design Summary
- Monolithic Kernel Design
- Single User
- Hybrid Scheduler
- Custom Syscall interface

## Motivation
- Workstation Focus
  - Modern operating system focus has shifted from getting work done locally on workstation type devices to cloud workloads. That trend is nothing new workloads shift between centralized and decentralized periodically (terminal/mainframe, Terminal servers, Virtual Desktops, etc). However workstation systems share operating systems with servers whether you are using Windows or Linux. This means that trade-offs at the design level will favor server workloads.
