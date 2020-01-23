/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019  Michael Fabian Dirks <info@xaymar.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "overlapped-queue.hpp"

datapath::windows::overlapped_queue::overlapped_queue(size_t backlog)
{
	std::unique_lock<std::mutex> ul(this->objs_lock);
	for (size_t idx = 0; idx < backlog; idx++) {
		free_objs.push(std::make_shared<datapath::windows::overlapped>());
	}
}

datapath::windows::overlapped_queue::~overlapped_queue()
{
	{
		std::unique_lock<std::mutex> ul(this->objs_lock);
		while (free_objs.size() > 0) {
			free_objs.pop();
		}
		used_objs.clear();
	}
}

std::shared_ptr<datapath::windows::overlapped> datapath::windows::overlapped_queue::alloc()
{
	std::shared_ptr<datapath::windows::overlapped> obj;
	std::unique_lock<std::mutex>                   ul(this->objs_lock);

	if (free_objs.size() > 0) {
		obj = free_objs.front();
		free_objs.pop();
	} else {
		obj = std::make_shared<datapath::windows::overlapped>();
	}

	used_objs.push_back(obj);
	return obj;
}

void datapath::windows::overlapped_queue::free(std::shared_ptr<datapath::windows::overlapped> overlapped)
{
	std::unique_lock<std::mutex> ul(this->objs_lock);
	for (auto itr = used_objs.begin(); itr != used_objs.end(); itr++) {
		if (*itr == overlapped) {
			used_objs.erase(itr);
			break;
		}
	}
	free_objs.push(overlapped);
}

/*
			// Security Descriptor Stuff
			SECURITY_ATTRIBUTES      security_attributes;
			PSECURITY_DESCRIPTOR     security_descriptor_ptr = NULL;
			PSID                     sid_everyone_ptr        = NULL;
			PSID                     sid_admin_ptr           = NULL;
			PACL                     acl_ptr                 = NULL;
			EXPLICIT_ACCESS          explicit_access[2];
			SID_IDENTIFIER_AUTHORITY sid_auth_world = SECURITY_WORLD_SID_AUTHORITY;
			SID_IDENTIFIER_AUTHORITY sid_auth_nt    = SECURITY_NT_AUTHORITY;

bool datapath::windows::overlapped_queue::create_security_attributes()
{
	DWORD dwRes;

	// Create a well-known SID for the Everyone group.
	if (!AllocateAndInitializeSid(&sid_auth_world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &sid_everyone_ptr)) {
		return false;
	}

	// Initialize an EXPLICIT_ACCESS structure for an ACE.
	// The ACE will allow Everyone read access to the key.
	ZeroMemory(&explicit_access, 2 * sizeof(EXPLICIT_ACCESS));
	explicit_access[0].grfAccessPermissions = KEY_READ;
	explicit_access[0].grfAccessMode        = SET_ACCESS;
	explicit_access[0].grfInheritance       = NO_INHERITANCE;
	explicit_access[0].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
	explicit_access[0].Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
	explicit_access[0].Trustee.ptstrName    = (LPTSTR)sid_everyone_ptr;

	// Create a SID for the BUILTIN\Administrators group.
	if (!AllocateAndInitializeSid(&sid_auth_nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0,
	                              0, 0, &sid_admin_ptr)) {
		return false;
	}

	// Initialize an EXPLICIT_ACCESS structure for an ACE.
	// The ACE will allow the Administrators group full access to
	// the key.
	explicit_access[1].grfAccessPermissions = KEY_ALL_ACCESS;
	explicit_access[1].grfAccessMode        = SET_ACCESS;
	explicit_access[1].grfInheritance       = NO_INHERITANCE;
	explicit_access[1].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
	explicit_access[1].Trustee.TrusteeType  = TRUSTEE_IS_GROUP;
	explicit_access[1].Trustee.ptstrName    = (LPTSTR)sid_admin_ptr;

	// Create a new ACL that contains the new ACEs.
	dwRes = SetEntriesInAcl(2, explicit_access, NULL, &acl_ptr);
	if (ERROR_SUCCESS != dwRes) {
		return false;
	}

	// Initialize a security descriptor.
	security_descriptor_ptr = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	if (NULL == security_descriptor_ptr) {
		return false;
	}

	if (!InitializeSecurityDescriptor(security_descriptor_ptr, SECURITY_DESCRIPTOR_REVISION)) {
		return false;
	}

	// Add the ACL to the security descriptor.
	if (!SetSecurityDescriptorDacl(security_descriptor_ptr,
	                               TRUE, // bDaclPresent flag
	                               acl_ptr,
	                               FALSE)) // not a default DACL
	{
		return false;
	}

	// Initialize a security attributes structure.
	security_attributes.nLength              = sizeof(SECURITY_ATTRIBUTES);
	security_attributes.lpSecurityDescriptor = security_descriptor_ptr;
	security_attributes.bInheritHandle       = FALSE;
	return true;
}

void datapath::windows::overlapped_queue::destroy_security_attributes()
{
	if (sid_everyone_ptr)
		FreeSid(sid_everyone_ptr);
	if (sid_admin_ptr)
		FreeSid(sid_admin_ptr);
	if (acl_ptr)
		LocalFree(acl_ptr);
	if (security_descriptor_ptr)
		LocalFree(security_descriptor_ptr);
}

*/