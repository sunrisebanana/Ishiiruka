// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/EXI_DeviceSlippi.h"

#include <array>
#include <unordered_map>
#include <stdexcept>

#include "SlippiLib/SlippiGame.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

CEXISlippi::CEXISlippi() {
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI Constructor called.");
}

CEXISlippi::~CEXISlippi() {
	closeFile();
}

void CEXISlippi::configureCommands(u8* payload, u8 length) {
	for (int i = 1; i < length; i += 3) {
		// Go through the receive commands payload and set up other commands
		u8 commandByte = payload[i];
		u32 commandPayloadSize = payload[i + 1] << 8 | payload[i + 2];
		payloadSizes[commandByte] = commandPayloadSize;
	}
}

std::vector<u8> CEXISlippi::generateMetadata() {
	std::vector<u8> metadata(
		{ 'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{' }
	);

	// TODO: Abstract out UBJSON functions to make this cleaner

	// Add game start time
	uint8_t dateTimeStrLength = sizeof "2011-10-08T07:07:09Z";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%FT%TZ", gmtime(&gameStartTime));
	dateTimeBuf.pop_back(); // Removes the \0 from the back of string
	metadata.insert(metadata.end(), { 
		'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (uint8_t)dateTimeBuf.size()
	});
	metadata.insert(metadata.end(), dateTimeBuf.begin(), dateTimeBuf.end());

	// Indicate this was played on dolphin
	metadata.insert(metadata.end(), {
		'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U',
		7, 'd', 'o', 'l', 'p', 'h', 'i', 'n'
	});

	// TODO: Add player names

	metadata.push_back('}');
	return metadata;
}

void CEXISlippi::writeToFile(u8* payload, u32 length, std::string fileOption) {
	std::vector<u8> dataToWrite;
	if (fileOption == "create") {
		// If the game sends over option 1 that means a file should be created
		createNewFile();

		// Start ubjson file and prepare the "raw" element that game
		// data output will be dumped into. The size of the raw output will
		// be initialized to 0 until all of the data has been received
		std::vector<u8> headerBytes(
			{ '{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0 }
		);
		dataToWrite.insert(dataToWrite.end(), headerBytes.begin(), headerBytes.end());

		// Used to keep track of how many bytes have been written to the file
		writtenByteCount = 0;
	}

	// If no file, do nothing
	if (!m_file) {
		return;
	}

	// Add the payload to data to write
	dataToWrite.insert(dataToWrite.end(), payload, payload + length);
	writtenByteCount += length;

	// If we are going to close the file, generate data to complete the UBJSON file
	if (fileOption == "close") {
		// This option indicates we are done sending over body
		std::vector<u8> closingBytes = generateMetadata();
		closingBytes.push_back('}');
		dataToWrite.insert(dataToWrite.end(), closingBytes.begin(), closingBytes.end());
	}

	// Write data to file
	bool result = m_file.WriteBytes(&dataToWrite[0], dataToWrite.size());
	if (!result) {
		ERROR_LOG(EXPANSIONINTERFACE, "Failed to write data to file.");
	}

	// If file should be closed, close it
	if (fileOption == "close") {
		// Write the number of bytes for the raw output
		u8 sizeByte0 = writtenByteCount >> 24;
		u8 sizeByte1 = (writtenByteCount & 0xFF0000) >> 16;
		u8 sizeByte2 = (writtenByteCount & 0xFF00) >> 8;
		u8 sizeByte3 = writtenByteCount & 0xFF;
		m_file.Seek(11, 0);
		std::vector<u8> sizeBytes({ sizeByte0, sizeByte1, sizeByte2, sizeByte3 });
		m_file.WriteBytes(&sizeBytes[0], sizeBytes.size());

		// Close file
		closeFile();
	}
}

void CEXISlippi::createNewFile() {
	if (m_file) {
		// If there's already a file open, close that one
		closeFile();
	}

	File::CreateDir("Slippi");
	std::string filepath = generateFileName();
	m_file = File::IOFile(filepath, "wb");
}

std::string CEXISlippi::generateFileName() {
	// Add game start time
	uint8_t dateTimeStrLength = sizeof "20171015T095717";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%Y%m%dT%H%M%S", localtime(&gameStartTime));

	std::string str(&dateTimeBuf[0]);
	return StringFromFormat("Slippi/Game_%s.slp", str.c_str());
}

void CEXISlippi::closeFile() {
	if (!m_file) {
		// If we have no file or payload is not game end, do nothing
		return;
	}

	// If this is the end of the game end payload, reset the file so that we create a new one
	m_file.Close();
	m_file = nullptr;
}

void CEXISlippi::loadFile(std::string path) {
	m_current_game = Slippi::SlippiGame::FromFile((std::string)path);
}

void CEXISlippi::prepareGameInfo() {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	Slippi::GameSettings* settings = m_current_game->GetSettings();

	// Build a word containing the stage and the presence of the characters
	u32 randomSeed = settings->randomSeed;
	m_read_queue.push_back(randomSeed);

	// This is kinda dumb but we need to handle the case where a player transforms
	// into sheik/zelda immediately. This info is not stored in the game info header
	// and so let's overwrite those values
	int player1Pos = 24; // This is the index of the first players character info
	std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> gameInfoHeader = settings->header;
	for (int i = 0; i < 4; i++) {
		// check if this player is actually in the game
		bool playerExists = m_current_game->DoesPlayerExist(i);
		if (!playerExists) {
			continue;
		}

		// check if the player is playing sheik or zelda
		uint8_t externalCharId = settings->players[i].characterId;
		if (externalCharId != 0x12 && externalCharId != 0x13) {
			continue;
		}

		// this is the position in the array that this player's character info is stored
		int pos = player1Pos + (9 * i);

		// here we have determined the player is playing sheik or zelda...
		// at this point let's overwrite the player's character with the one
		// that they are playing
		gameInfoHeader[pos] &= 0x00FFFFFF;
		gameInfoHeader[pos] |= externalCharId << 24;
	}

	// Write entire header to game
	for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; i++) {
		m_read_queue.push_back(gameInfoHeader[i]);
	}
}

void CEXISlippi::prepareFrameData(u8* payload) {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	int32_t frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	uint8_t port = payload[4];
	uint8_t isFollower = payload[5];

	// Load the data from this frame into the read buffer
	bool frameExists = m_current_game->DoesFrameExist(frameIndex);
	if (!frameExists) {
		return;
	}

	Slippi::FrameData* frame = m_current_game->GetFrame(frameIndex);

	// Add random seed to the front of the response regardless of player
	m_read_queue.push_back(*(u32*)&frame->randomSeed);

	std::unordered_map<uint8_t, Slippi::PlayerFrameData> source;
	source = isFollower ? frame->followers : frame->players;

	// Check if player exists
	if (!source.count(port)) {
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = source[port];

	// Add all of the inputs in order
	m_read_queue.push_back(*(u32*)&data.joystickX);
	m_read_queue.push_back(*(u32*)&data.joystickY);
	m_read_queue.push_back(*(u32*)&data.cstickX);
	m_read_queue.push_back(*(u32*)&data.cstickY);
	m_read_queue.push_back(*(u32*)&data.trigger);
	m_read_queue.push_back(data.buttons);
	m_read_queue.push_back(*(u32*)&data.locationX);
	m_read_queue.push_back(*(u32*)&data.locationY);
	m_read_queue.push_back(*(u32*)&data.facingDirection);
}

void CEXISlippi::prepareLocationData(u8* payload) {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	int32_t frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	uint8_t port = payload[4];
	uint8_t isFollower = payload[5];

	// Load the data from this frame into the read buffer
	bool frameExists = m_current_game->DoesFrameExist(frameIndex);
	if (!frameExists) {
		return;
	}

	Slippi::FrameData* frame = m_current_game->GetFrame(frameIndex);

	std::unordered_map<uint8_t, Slippi::PlayerFrameData> source;
	source = isFollower ? frame->followers : frame->players;

	if (!source.count(port)) {
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = source[port];

	// Add all of the inputs in order
	m_read_queue.push_back(*(u32*)&data.locationX);
	m_read_queue.push_back(*(u32*)&data.locationY);
	m_read_queue.push_back(*(u32*)&data.facingDirection);
}

void CEXISlippi::ImmWrite(u32 data, u32 size)
{
	//init();
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmWrite: %08x, size: %d", data, size);

	bool lookingForMessage = m_payload_type == CMD_UNKNOWN;
	if (lookingForMessage) {
		// If the size is not one, this can't be the start of a command
		if (size != 1) {
			return;
		}

		m_payload_type = data >> 24;

		// Attempt to get payload size for this command. If not found, don't do anything
		if (!payloadSizes.count(m_payload_type)) {
			m_payload_type = CMD_UNKNOWN;
			return;
		}
	}

	// Read and incremement our payload location
	m_payload_loc += size;

	// Add new data to payload
	for (u32 i = 0; i < size; i++) {
		int shiftAmount = 8 * (3 - i);
		u8 byte = 0xFF & (data >> shiftAmount);
		m_payload.push_back(byte);
	}

	// This section deals with saying we are done handling the payload
	// add one because we count the command as part of the total size
	u32 payloadSize = payloadSizes[m_payload_type];
	if (m_payload_type == CMD_RECEIVE_COMMANDS && m_payload_loc > 1) {
		// the receive commands command tells us exactly how long it is
		// this is to make adding new commands easier
		payloadSize = m_payload[1];
	}

	if (m_payload_loc >= payloadSize + 1) {
		// Handle payloads
		switch (m_payload_type) {
		case CMD_RECEIVE_COMMANDS:
			time(&gameStartTime); // Store game start time
			configureCommands(&m_payload[1], m_payload_loc - 1);
			writeToFile(&m_payload[0], m_payload_loc, "create");
			break;
		case CMD_RECEIVE_GAME_END:
			writeToFile(&m_payload[0], m_payload_loc, "close");
			break;
		case CMD_PREPARE_REPLAY:
			loadFile("Slippi/CurrentGame.slp");
			prepareGameInfo();
			break;
		case CMD_READ_FRAME:
			prepareFrameData(&m_payload[1]);
			break;
		case CMD_GET_LOCATION:
			prepareLocationData(&m_payload[1]);
			break;
		default:
			writeToFile(&m_payload[0], m_payload_loc, "");
			break;
		}

		// reset payload loc and type so we look for next command
		m_payload_loc = 0;
		m_payload_type = CMD_UNKNOWN;
		m_payload.clear();
	}
}

u32 CEXISlippi::ImmRead(u32 size)
{
	if (m_read_queue.empty()) {
		INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmRead: Empty");
		return 0;
	}

	u32 value = m_read_queue.front();
	m_read_queue.pop_front();

	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmRead %08x", value);

	return value;
}

bool CEXISlippi::IsPresent() const
{
	return true;
}

void CEXISlippi::TransferByte(u8& byte)
{
}