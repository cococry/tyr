cp ./reifconfig.mk ./vendor/reif/config.mk	
cd vendor/reif
./install.sh
cd ../..
make -B 
sudo make install
