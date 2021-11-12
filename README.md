# embedded-protocol-sample
Illustrative embedded protocol 

Simple embedded protocol implemented as threads for illustrating the consequences of different design decisions. The protocol is aimed for stream-like underlying layers like UART or TCP. The sample shows the usage of [mmfl](https://github.com/gega/mmfl) message framing library and also the [ccsv](https://github.com/gega/ccsv) csv parser library. The design goal is to make a robust protocol with minimal maintenance and relatively easy extension. This is a semi-asynchronous style protocol which allows query/response type commands and also slow, background commands with delayed results.
