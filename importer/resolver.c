#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ipfs/importer/resolver.h"
#include "libp2p/crypto/encoding/base58.h"
#include "ipfs/merkledag/node.h"
#include "ipfs/merkledag/merkledag.h"
#include "ipfs/repo/fsrepo/fs_repo.h"
#include "libp2p/net/multistream.h"
#include "libp2p/record/message.h"
#include "libp2p/utils/multiaddress.h"

/**
 * return the next chunk of a path
 * @param path the path
 * @param next_part a pointer to a string NOTE: don't forget to free
 * @returns true(1) on success, false(0) on error, or no more parts
 */
int ipfs_resolver_next_path(const char* path, char** next_part) {
	for (int i = 0; i < strlen(path); i++) {
		if (path[i] != '/') { // we have the next section
			char* pos = strchr(&path[i+1], '/');
			if (pos == NULL) {
				*next_part = (char*)malloc(strlen(path) + 1);
				strcpy(*next_part, path);
			} else {
				*next_part = (char*)malloc(pos - &path[i] + 1);
				strncpy(*next_part, &path[i], pos-&path[i]);
				(*next_part)[pos-&path[i]] = 0;
			}
			return 1;
		}
	}
	return 0;
}

/**
 * Remove preceding slash and "/ipfs/" or "/ipns/" as well as the local multihash (if it is local)
 * @param path the path from the command line
 * @param fs_repo the local repo
 * @returns the modified path
 */
const char* ipfs_resolver_remove_path_prefix(const char* path, const struct FSRepo* fs_repo) {
	int pos = 0;
	int first_non_slash = -1;
	while(&path[pos] != NULL) {
		if (path[pos] == '/') {
			pos++;
			continue;
		} else {
			if (first_non_slash == -1)
				first_non_slash = pos;
			if (pos == first_non_slash && (strncmp(&path[pos], "ipfs", 4) == 0 || strncmp(&path[pos], "ipns", 4) == 0) ) {
				// ipfs or ipns should be up front. Otherwise, it could be part of the path
				pos += 4;
			} else if (strncmp(&path[pos], fs_repo->config->identity->peer_id, strlen(fs_repo->config->identity->peer_id)) == 0) {
				pos += strlen(fs_repo->config->identity->peer_id) + 1; // the slash
			} else {
				return &path[pos];
			}
		}
	}
	return NULL;
}

/**
 * Determine if this path is a remote path
 * @param path the path to examine
 * @param fs_repo the local repo
 * @returns true(1) if this path is a remote path
 */
int ipfs_resolver_is_remote(const char* path, const struct FSRepo* fs_repo) {
	int pos = 0;

	// skip the first slash
	while (&path[pos] != NULL && path[pos] == '/') {
		pos++;
	}
	if (&path[pos] == NULL)
		return 0;

	// skip the ipfs prefix
	if (strncmp(&path[pos], "ipfs/", 5) == 0 || strncmp(&path[pos], "ipns/", 5) == 0) {
		pos += 5; //the word plus the slash
	} else
		return 0;

	// if this is a Qm code, see if it is a local Qm code
	if (path[pos] == 'Q' && path[pos+1] == 'm') {
		if (strncmp(&path[pos], fs_repo->config->identity->peer_id, strlen(fs_repo->config->identity->peer_id)) != 0) {
			return 1;
		}
	}
	return 0;

}

/**
 * Retrieve a node from a remote source
 * @param path the path to retrieve
 * @param from where to start
 * @param fs_repo the local repo
 * @returns the node, or NULL if not found
 */
struct Node* ipfs_resolver_remote_get(const char* path, struct Node* from, const struct IpfsNode* ipfs_node) {
	// parse the path
	const char* temp = ipfs_resolver_remove_path_prefix(path, ipfs_node->repo);
	if (temp == NULL)
		return NULL;
	char* pos = strchr(temp, '/');
	if (pos == NULL || pos - temp > 254)
		return NULL;
	char id[255];
	size_t id_size = pos - temp;
	strncpy(id, temp, id_size);
	id[id_size] = 0;
	char* key = &pos[1];
	pos = strchr(key, '/');
	if (pos == NULL || pos - key > 254)
		return NULL;
	pos[0] = '\0';
	// get the multiaddress for this
	struct Libp2pPeer* peer = libp2p_peerstore_get_peer(ipfs_node->peerstore, (unsigned char*)id, id_size);
	if (peer == NULL)
		return NULL;
	// connect to the peer
	struct MultiAddress* address = peer->addr_head->item;
	char* ip;
	int port;
	libp2p_utils_multiaddress_parse_ip4_tcp(address, &ip, &port);
	struct Stream* stream = libp2p_net_multistream_connect(ip, port);
	free(ip);
	// build the request
	struct Libp2pMessage* message = libp2p_message_new();
	message->message_type = MESSAGE_TYPE_GET_VALUE;
	message->key = key;
	message->key_size = strlen(key);
	size_t message_protobuf_size = libp2p_message_protobuf_encode_size(message);
	unsigned char message_protobuf[message_protobuf_size];
	libp2p_message_protobuf_encode(message, message_protobuf, message_protobuf_size, &message_protobuf_size);
	stream->write(stream, message_protobuf, message_protobuf_size);
	unsigned char* response;
	size_t response_size;
	// we should get back a protobuf'd record
	stream->read(stream, &response, &response_size);
	if (response_size == 1)
		return NULL;
	// turn the protobuf into a Node
	struct Node* node;
	ipfs_node_protobuf_decode(response, response_size, &node);
	return node;
}

/**
 * Interogate the path and the current node, looking
 * for the desired node.
 * @param path the current path
 * @param from the current node (or NULL if it is the first call)
 * @returns what we are looking for, or NULL if it wasn't found
 */
struct Node* ipfs_resolver_get(const char* path, struct Node* from, const struct IpfsNode* ipfs_node) {

	struct FSRepo* fs_repo = ipfs_node->repo;

	// shortcut for remote files
	if (from == NULL && ipfs_resolver_is_remote(path, fs_repo)) {
		return ipfs_resolver_remote_get(path, from, ipfs_node);
	}
	/**
	 * Memory management notes:
	 * If we find what we're looking for, we clean up "from" and return the object
	 * If we don't find what we're looking for, but we can continue the search, we clean up "from"
	 * If we don't find what we're looking for, and we cannot continue, we do not clean up "from"
	 */
	// remove unnecessary stuff
	if (from == NULL)
		path = ipfs_resolver_remove_path_prefix(path, fs_repo);
	// grab the portion of the path to work with
	char* path_section;
	if (ipfs_resolver_next_path(path, &path_section) == 0)
		return NULL;
	struct Node* current_node = NULL;
	if (from == NULL) {
		// this is the first time around. Grab the root node
		if (path_section[0] == 'Q' && path_section[1] == 'm') {
			// we have a hash. Convert to a real hash, and find the node
			size_t hash_length = libp2p_crypto_encoding_base58_decode_size(strlen(path_section));
			unsigned char hash[hash_length];
			unsigned char* ptr = &hash[0];
			if (libp2p_crypto_encoding_base58_decode((unsigned char*)path_section, strlen(path_section), &ptr, &hash_length) == 0) {
				free(path_section);
				return NULL;
			}
			if (ipfs_merkledag_get_by_multihash(hash, hash_length, &current_node, fs_repo) == 0) {
				free(path_section);
				return NULL;
			}
			// we have the root node, now see if we want this or something further down
			int pos = strlen(path_section);
			if (pos == strlen(path)) {
				free(path_section);
				return current_node;
			} else {
				// look on...
				free(path_section);
				struct Node* newNode = ipfs_resolver_get(&path[pos+1], current_node, ipfs_node); // the +1 is the slash
				return newNode;
			}
		} else {
			// we don't have a current node, and we don't have a hash. Something is wrong
			free(path_section);
			return NULL;
		}
	} else {
		// we were passed a node. If it is a directory, see if what we're looking for is in it
		if (ipfs_node_is_directory(from)) {
			struct NodeLink* curr_link = from->head_link;
			while (curr_link != NULL) {
				// if it matches the name, we found what we're looking for.
				// If so, load up the node by its hash
				if (strcmp(curr_link->name, path_section) == 0) {
					if (ipfs_merkledag_get(curr_link->hash, curr_link->hash_size, &current_node, fs_repo) == 0) {
						free(path_section);
						return NULL;
					}
					if (strlen(path_section) == strlen(path)) {
						// we are at the end of our search
						ipfs_node_free(from);
						free(path_section);
						return current_node;
					} else {
						char* next_path_section;
						ipfs_resolver_next_path(&path[strlen(path_section)], &next_path_section);
						free(path_section);
						// if we're at the end of the path, return the node
						// continue looking for the next part of the path
						ipfs_node_free(from);
						struct Node* newNode = ipfs_resolver_get(next_path_section, current_node, ipfs_node);
						return newNode;
					}
				}
				curr_link = curr_link->next;
			}
		} else {
			// we're asking for a file from an object that is not a directory. Bail.
			free(path_section);
			return NULL;
		}
	}
	// it should never get here
	free(path_section);
	if (from != NULL)
		ipfs_node_free(from);
	return NULL;
}
