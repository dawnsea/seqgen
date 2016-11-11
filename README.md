# seqgen

## who
dawnsea
keeptalk@gmail.com
http://www.troot.co.kr/ (Korean)

## what
high speed sequence number generator

## build
make

## library dependency
event, pthread

## how
./seqgen
  + -p [portnumber]
  + -i [initial value]
  + -e [initial epoch value]
  + [mode]
	  + -h // http
	  + -s // tcp socket
	  + -c // memcached compatible (incr command)
  + -n // keepalive off

## log
syslog, domain : seqgen

## default value

|value | default |
|-----|-----|
|port | 5555 |
|keepalive cycle | 1000|
|keepalive timeout | 5 second |
|worker | 100 thread |

## test

http : open browser and go http://localhost:5555/
*note1* : The seq number will be increase double because of request of 'favicon.ico'.
socket : go telnet localhost 5555, and press enter.
memcached compatible : use memcached client library, and request 'incr' memcached command. or telnet localhost 5555, and press enter

## performance test
http : use apache ab or jmeter. ex) ab -c 100 -n 10000 http://localhost:5555/
socket or memcached : Do it yourself.


## to do

master - slave mode.
if this prj. takes 2 star, I will make the feature for fail-over.

## referrece
"Multithreaded, libevent-based socket server.", README, LICENSE.txt, Ronald B. Cemer, 2012

## license

BSD and read the REAME and LICENSE.txt
