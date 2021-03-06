#pragma once

/**
 * get a file by its hash, and write the data to a file
 * @param hash the base58 multihash of the cid
 * @param file_name the file name to write to
 * @returns true(1) on success
 */
int ipfs_exporter_to_file(const unsigned char* hash, const char* file_name, const struct FSRepo* fs_repo);

int ipfs_exporter_object_get(int argc, char** argv);

/***
 * Called from the command line with ipfs cat [hash]. Retrieves the object pointed to by hash, and displays its block data (links and data elements)
 * @param argc number of arguments
 * @param argv arguments
 * @returns true(1) on success
 */
int ipfs_exporter_object_cat(int argc, char** argv);
