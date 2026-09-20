#!/bin/sh

numargs=$#
if [ $numargs -lt 2 ]
then 
	echo Args not specified
	exit 1
fi

filesdir=$1

if [ ! -d "$filesdir" ]
then
	echo Seach Directory does not exist
	exit 1
fi


searchstr=$2

if [ -z "$searchstr" ]
then 
	echo string to search was empty
	exit 1
fi
#Search Files for specific string
X=$(grep -l -r "$searchstr" "$filesdir" | wc -l)
Y=$(grep -c -r "$searchstr" "$filesdir" | awk -F: '{sum += $NF} END  {print sum+0}')

echo The number of files are $X and the number of matching lines are $Y
