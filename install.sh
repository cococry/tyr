#!/bin/bash

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print welcome message
echo -e "${GREEN}============================================="
echo -e "${GREEN}     Installing TYR - Step-by-Step"
echo -e "${GREEN}=============================================${NC}"

# Step 1: Clone the repository into vendor/reif if the directory doesn't exist
if [ ! -d "vendor/reif" ]; then
    echo -e "${YELLOW}Cloning the REIF repository...${NC}"
    if git clone https://github.com/cococry/reif vendor/reif; then
        echo -e "${GREEN}Successfully cloned REIF repository!${NC}"
    else
        echo -e "${RED}Failed to clone the REIF repository. Please check your internet connection and try again.${NC}"
        exit 1
    fi
else
    echo -e "${GREEN}REIF repository already exists in vendor/reif, skipping clone.${NC}"
fi

# Step 2: Copy the config file
echo -e "${YELLOW}Copying config file...${NC}"
if cp ./reifconfig.mk ./vendor/reif/config.mk; then
    echo -e "${GREEN}Config file copied successfully!${NC}"
else
    echo -e "${RED}Failed to copy the config file. Please check if the source file exists.${NC}"
    exit 1
fi

# Step 3: Run the REIF installation script
echo -e "${YELLOW}Running the REIF installation script...${NC}"
cd vendor/reif
if ./install.sh; then
    echo -e "${GREEN}REIF installation script completed successfully!${NC}"
else
    echo -e "${RED}REIF installation script failed. Please check the logs above.${NC}"
    exit 1
fi
cd ../..

# Step 4: Run the `make` command with the `-B` option to force rebuild
echo -e "${YELLOW}Running 'make -B' to rebuild everything...${NC}"
if make -B; then
    echo -e "${GREEN}Build completed successfully!${NC}"
else
    echo -e "${RED}Build failed. Please check for errors above.${NC}"
    exit 1
fi

# Step 5: Install the software
echo -e "${YELLOW}Installing the software...${NC}"
if sudo make install; then
    echo -e "${GREEN}Installation successful!${NC}"
else
    echo -e "${RED}Installation failed. Please check for errors above.${NC}"
    exit 1
fi

# Final message
echo -e "${GREEN}============================================="
echo -e "${GREEN}     TYR installation completed!"
echo -e "${GREEN}=============================================${NC}"
