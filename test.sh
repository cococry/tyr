while true
do
  ./bin/tyr &  
  sleep 1s 
  pkill tyr 
done
