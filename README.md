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

## default value

|value | default |
|-----|-----|
|port | 5555 |
|keepalive cycle | 1000|
|keepalive timeout | 5 second |
|worker | 100 thread |

## to do

master - slave mode.
if this prj. takes 2 star, I will make the feature for fail-over.

## referrece
"Multithreaded, libevent-based socket server.", README, LICENSE.txt, Ronald B. Cemer, 2012

## license

BSD and read the REAME and LICENSE.txt
