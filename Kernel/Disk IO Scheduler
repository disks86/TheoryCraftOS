# Disk I/O Scheduler
The Disk I/O scheduler is really two schedulers a trusted and untrusted scheduler. Disk requests are processed in order from the trusted queue. If there are no requests in the trusted queue then one request will be processed from one of the untrusted queues.

## Untrusted Disk I/O Scheduler
The untrusted scheduler is a round-robin scheduler where requests are divided by processes. One request is processed from each processes queue before starting over at the start of the list of queues. In general this sheduler is designed to fairly split Disk access between application.

## Trusted Disk I/O Scheduler
The trusted disk I/O Scheduler is a simple in order FIFO queue. This queue is designed with flash media and other storage systems with their own controllers and high random access performance. Requests are not combined which may result in lower performance on some older hardware.
