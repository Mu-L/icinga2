/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/apilistener.hpp"
#include "remote/apifunction.hpp"
#include "remote/configobjectutility.hpp"
#include "remote/jsonrpc.hpp"
#include "base/configtype.hpp"
#include "base/convert.hpp"
#include "base/dependencygraph.hpp"
#include "base/json.hpp"
#include "config/vmops.hpp"
#include "remote/configobjectslock.hpp"
#include <fstream>
#include <unordered_set>

using namespace icinga;

REGISTER_APIFUNCTION(UpdateObject, config, &ApiListener::ConfigUpdateObjectAPIHandler);
REGISTER_APIFUNCTION(DeleteObject, config, &ApiListener::ConfigDeleteObjectAPIHandler);

INITIALIZE_ONCE([]() {
	ConfigObject::OnActiveChanged.connect(&ApiListener::ConfigUpdateObjectHandler);
	ConfigObject::OnVersionChanged.connect(&ApiListener::ConfigUpdateObjectHandler);
});

void ApiListener::ConfigUpdateObjectHandler(const ConfigObject::Ptr& object, const Value& cookie)
{
	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (!listener)
		return;

	if (object->IsActive()) {
		/* Sync object config */
		listener->UpdateConfigObject(object, cookie);
	} else if (!object->IsActive() && object->GetExtension("ConfigObjectDeleted")) {
		/* Delete object */
		listener->DeleteConfigObject(object, cookie);
	}
}

Value ApiListener::ConfigUpdateObjectAPIHandler(const MessageOrigin::Ptr& origin, const Dictionary::Ptr& params)
{
	Log(LogNotice, "ApiListener")
		<< "Received config update for object: " << JsonEncode(params);

	/* check permissions */
	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (!listener)
		return Empty;

	String objType = params->Get("type");
	String objName = params->Get("name");

	Endpoint::Ptr endpoint = origin->FromClient->GetEndpoint();

	String identity = origin->FromClient->GetIdentity();

	/* discard messages if the client is not configured on this node */
	if (!endpoint) {
		Log(LogNotice, "ApiListener")
			<< "Discarding 'config update object' message from '" << identity << "': Invalid endpoint origin (client not allowed).";
		return Empty;
	}

	Zone::Ptr endpointZone = endpoint->GetZone();

	/* discard messages if the sender is in a child zone */
	if (!Zone::GetLocalZone()->IsChildOf(endpointZone)) {
		Log(LogNotice, "ApiListener")
			<< "Discarding 'config update object' message"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << objName << "' of type '" << objType << "'. Sender is in a child zone.";
		return Empty;
	}

	String objZone = params->Get("zone");

	if (!objZone.IsEmpty() && !Zone::GetByName(objZone)) {
		Log(LogNotice, "ApiListener")
			<< "Discarding 'config update object' message"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << objName << "' of type '" << objType << "'. Objects zone '" << objZone << "' isn't known locally.";
		return Empty;
	}

	/* ignore messages if the endpoint does not accept config */
	if (!listener->GetAcceptConfig()) {
		Log(LogWarning, "ApiListener")
			<< "Ignoring config update"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << objName << "' of type '" << objType << "'. '" << listener->GetName() << "' does not accept config.";
		return Empty;
	}

	/* update the object */
	double objVersion = params->Get("version");

	Type::Ptr ptype = Type::GetByName(objType);
	auto *ctype = dynamic_cast<ConfigType *>(ptype.get());

	if (!ctype) {
		// This never happens with icinga cluster endpoints, only with development errors.
		Log(LogCritical, "ApiListener")
			<< "Config type '" << objType << "' does not exist.";
		return Empty;
	}

	// Wait for the object name to become available for processing and block it immediately.
	// Doing so guarantees that only one (create/update/delete) cluster event or API request of a
	// given object is being processed at any given time.
	ObjectNameLock objectNameLock(ptype, objName);

	ConfigObject::Ptr object = ctype->GetObject(objName);

	String config = params->Get("config");

	bool newObject = false;

	if (!object && !config.IsEmpty()) {
		newObject = true;

		/* object does not exist, create it through the API */
		Array::Ptr errors = new Array();

		/*
		 * Create the config object through our internal API.
		 * IMPORTANT: Pass the origin to prevent cluster sync loops.
		 */
		if (!ConfigObjectUtility::CreateObject(ptype, objName, config, errors, nullptr, origin)) {
			Log(LogCritical, "ApiListener")
				<< "Could not create object '" << objName << "':";

			ObjectLock olock(errors);
			for (auto& error : errors) {
				Log(LogCritical, "ApiListener", error);
			}

			return Empty;
		}

		object = ctype->GetObject(objName);

		if (!object)
			return Empty;

		/* object was created, update its version */
		object->SetVersion(objVersion, false, origin);
	}

	if (!object)
		return Empty;

	/* update object attributes if version was changed or if this is a new object */
	if (newObject || objVersion <= object->GetVersion()) {
		Log(LogNotice, "ApiListener")
			<< "Discarding config update"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << object->GetName()
			<< "': Object version " << std::fixed << object->GetVersion()
			<< " is more recent than the received version " << std::fixed << objVersion << ".";

		return Empty;
	}

	Log(LogNotice, "ApiListener")
		<< "Processing config update"
		<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
		<< " for object '" << object->GetName()
		<< "': Object version " << object->GetVersion()
		<< " is older than the received version " << objVersion << ".";

	Dictionary::Ptr modified_attributes = params->Get("modified_attributes");

	if (modified_attributes) {
		ObjectLock olock(modified_attributes);
		for (const Dictionary::Pair& kv : modified_attributes) {
			/* update all modified attributes
			 * but do not update the object version yet.
			 * This triggers cluster events otherwise.
			 */
			object->ModifyAttribute(kv.first, kv.second, false);
		}
	}

	/* check whether original attributes changed and restore them locally */
	Array::Ptr newOriginalAttributes = params->Get("original_attributes");
	Dictionary::Ptr objOriginalAttributes = object->GetOriginalAttributes();

	if (newOriginalAttributes && objOriginalAttributes) {
		std::vector<String> restoreAttrs;

		{
			ObjectLock xlock(objOriginalAttributes);
			for (const Dictionary::Pair& kv : objOriginalAttributes) {
				/* original attribute was removed, restore it */
				if (!newOriginalAttributes->Contains(kv.first))
					restoreAttrs.push_back(kv.first);
			}
		}

		for (const String& key : restoreAttrs) {
			/* do not update the object version yet. */
			object->RestoreAttribute(key, false);
		}
	}

	/* keep the object version in sync with the sender */
	object->SetVersion(objVersion, false, origin);

	return Empty;
}

Value ApiListener::ConfigDeleteObjectAPIHandler(const MessageOrigin::Ptr& origin, const Dictionary::Ptr& params)
{
	Log(LogNotice, "ApiListener")
		<< "Received config delete for object: " << JsonEncode(params);

	/* check permissions */
	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (!listener)
		return Empty;

	String objType = params->Get("type");
	String objName = params->Get("name");

	Endpoint::Ptr endpoint = origin->FromClient->GetEndpoint();

	String identity = origin->FromClient->GetIdentity();

	if (!endpoint) {
		Log(LogNotice, "ApiListener")
			<< "Discarding 'config delete object' message from '" << identity << "': Invalid endpoint origin (client not allowed).";
		return Empty;
	}

	Zone::Ptr endpointZone = endpoint->GetZone();

	/* discard messages if the sender is in a child zone */
	if (!Zone::GetLocalZone()->IsChildOf(endpointZone)) {
		Log(LogNotice, "ApiListener")
			<< "Discarding 'config delete object' message"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << objName << "' of type '" << objType << "'. Sender is in a child zone.";
		return Empty;
	}

	if (!listener->GetAcceptConfig()) {
		Log(LogWarning, "ApiListener")
			<< "Ignoring config delete"
			<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
			<< " for object '" << objName << "' of type '" << objType << "'. '" << listener->GetName() << "' does not accept config.";
		return Empty;
	}

	/* delete the object */
	Type::Ptr ptype = Type::GetByName(objType);
	auto *ctype = dynamic_cast<ConfigType *>(ptype.get());

	if (!ctype) {
		// This never happens with icinga cluster endpoints, only with development errors.
		Log(LogCritical, "ApiListener")
			<< "Config type '" << objType << "' does not exist.";
		return Empty;
	}

	// Wait for the object name to become available for processing and block it immediately.
	// Doing so guarantees that only one (create/update/delete) cluster event or API request of a
	// given object is being processed at any given time.
	ObjectNameLock objectNameLock(ptype, objName);

	ConfigObject::Ptr object = ctype->GetObject(objName);

	if (!object) {
		Log(LogNotice, "ApiListener")
			<< "Could not delete non-existent object '" << objName << "' with type '" << params->Get("type") << "'.";
		return Empty;
	}

	if (object->GetPackage() != "_api") {
		Log(LogCritical, "ApiListener")
			<< "Could not delete object '" << objName << "': Not created by the API.";
		return Empty;
	}

	Log(LogNotice, "ApiListener")
		<< "Processing config delete"
		<< " from '" << identity << "' (endpoint: '" << endpoint->GetName() << "', zone: '" << endpointZone->GetName() << "')"
		<< " for object '" << object->GetName() << "'.";

	Array::Ptr errors = new Array();

	/*
	 * Delete the config object through our internal API.
	 * IMPORTANT: Pass the origin to prevent cluster sync loops.
	 */
	if (!ConfigObjectUtility::DeleteObject(object, true, errors, nullptr, origin)) {
		Log(LogCritical, "ApiListener", "Could not delete object:");

		ObjectLock olock(errors);
		for (auto& error : errors) {
			Log(LogCritical, "ApiListener", error);
		}
	}

	return Empty;
}

void ApiListener::UpdateConfigObject(const ConfigObject::Ptr& object, const MessageOrigin::Ptr& origin,
	const JsonRpcConnection::Ptr& client)
{
	/* only send objects to zones which have access to the object */
	if (client) {
		Zone::Ptr target_zone = client->GetEndpoint()->GetZone();

		if (target_zone && !target_zone->CanAccessObject(object)) {
			Log(LogDebug, "ApiListener")
				<< "Not sending 'update config' message to unauthorized zone '" << target_zone->GetName() << "'"
				<< " for object: '" << object->GetName() << "'.";

			return;
		}
	}

	if (object->GetPackage() != "_api" && object->GetVersion() == 0)
		return;

	Dictionary::Ptr params = new Dictionary();

	Dictionary::Ptr message = new Dictionary({
		{ "jsonrpc", "2.0" },
		{ "method", "config::UpdateObject" },
		{ "params", params }
	});

	params->Set("name", object->GetName());
	params->Set("type", object->GetReflectionType()->GetName());
	params->Set("version", object->GetVersion());

	String zoneName = object->GetZoneName();

	if (!zoneName.IsEmpty())
		params->Set("zone", zoneName);

	if (object->GetPackage() == "_api") {
		std::ifstream fp(ConfigObjectUtility::GetExistingObjectConfigPath(object).CStr(), std::ifstream::binary);
		if (!fp)
			return;

		String content((std::istreambuf_iterator<char>(fp)), std::istreambuf_iterator<char>());
		params->Set("config", content);
	}

	Dictionary::Ptr original_attributes = object->GetOriginalAttributes();
	Dictionary::Ptr modified_attributes = new Dictionary();
	ArrayData newOriginalAttributes;

	if (original_attributes) {
		ObjectLock olock(original_attributes);
		for (const Dictionary::Pair& kv : original_attributes) {
			std::vector<String> tokens = kv.first.Split(".");

			Value value = object;
			for (const String& token : tokens) {
				value = VMOps::GetField(value, token);
			}

			modified_attributes->Set(kv.first, value);

			newOriginalAttributes.push_back(kv.first);
		}
	}

	params->Set("modified_attributes", modified_attributes);

	/* only send the original attribute keys */
	params->Set("original_attributes", new Array(std::move(newOriginalAttributes)));

#ifdef I2_DEBUG
	Log(LogDebug, "ApiListener")
		<< "Sent update for object '" << object->GetName() << "': " << JsonEncode(params);
#endif /* I2_DEBUG */

	if (client)
		client->SendMessage(message);
	else {
		Zone::Ptr target = static_pointer_cast<Zone>(object->GetZone());

		if (!target)
			target = Zone::GetLocalZone();

		RelayMessage(origin, target, message, false);
	}
}

/**
 * Syncs the specified object and its direct and indirect parents to the provided client
 * in topological order of their dependency graph recursively.
 *
 * Objects that the client does not have access to are skipped without going through their dependency graph.
 *
 * Please do not use this method to forward remote generated cluster updates; it should only be used to
 * send local updates to that specific non-nullptr client.
 *
 * @param object The config object you want to sync.
 * @param azone The zone of the client you want to send the update to.
 * @param client The JsonRpc client you send the update to.
 * @param syncedObjects Used to cache the already synced objects.
 */
void ApiListener::UpdateConfigObjectWithParents(const ConfigObject::Ptr& object, const Zone::Ptr& azone,
	const JsonRpcConnection::Ptr& client, std::unordered_set<ConfigObject*>& syncedObjects)
{
	if (syncedObjects.find(object.get()) != syncedObjects.end()) {
		return;
	}

	/* don't sync objects for non-matching parent-child zones */
	if (!azone->CanAccessObject(object)) {
		return;
	}
	syncedObjects.emplace(object.get());

	for (const auto& parent : DependencyGraph::GetParents(object)) {
		UpdateConfigObjectWithParents(parent, azone, client, syncedObjects);
	}

	/* send the config object to the connected client */
	UpdateConfigObject(object, nullptr, client);
}

void ApiListener::DeleteConfigObject(const ConfigObject::Ptr& object, const MessageOrigin::Ptr& origin,
	const JsonRpcConnection::Ptr& client)
{
	if (object->GetPackage() != "_api")
		return;

	/* only send objects to zones which have access to the object */
	if (client) {
		Zone::Ptr target_zone = client->GetEndpoint()->GetZone();

		if (target_zone && !target_zone->CanAccessObject(object)) {
			Log(LogDebug, "ApiListener")
				<< "Not sending 'delete config' message to unauthorized zone '" << target_zone->GetName() << "'"
				<< " for object: '" << object->GetName() << "'.";

			return;
		}
	}

	Dictionary::Ptr params = new Dictionary();

	Dictionary::Ptr message = new Dictionary({
		{ "jsonrpc", "2.0" },
		{ "method", "config::DeleteObject" },
		{ "params", params }
	});

	params->Set("name", object->GetName());
	params->Set("type", object->GetReflectionType()->GetName());
	params->Set("version", object->GetVersion());


#ifdef I2_DEBUG
	Log(LogDebug, "ApiListener")
		<< "Sent delete for object '" << object->GetName() << "': " << JsonEncode(params);
#endif /* I2_DEBUG */

	if (client)
		client->SendMessage(message);
	else {
		Zone::Ptr target = static_pointer_cast<Zone>(object->GetZone());

		if (!target)
			target = Zone::GetLocalZone();

		RelayMessage(origin, target, message, true);
	}
}

/* Initial sync on connect for new endpoints */
void ApiListener::SendRuntimeConfigObjects(const JsonRpcConnection::Ptr& aclient)
{
	Endpoint::Ptr endpoint = aclient->GetEndpoint();
	ASSERT(endpoint);

	Zone::Ptr azone = endpoint->GetZone();

	Log(LogInformation, "ApiListener")
		<< "Syncing runtime objects to endpoint '" << endpoint->GetName() << "'.";

	std::unordered_set<ConfigObject*> syncedObjects;
	for (const Type::Ptr& type : Type::GetAllTypes()) {
		if (auto *ctype = dynamic_cast<ConfigType *>(type.get())) {
			for (const auto& object : ctype->GetObjects()) {
				// All objects must be synced sorted by their dependency graph.
				// Otherwise, downtimes/comments etc. might get synced before their respective Checkables, which will
				// result in comments and downtimes being ignored by the other endpoint since it does not yet know
				// about their checkables. Given that the runtime config updates event does not trigger a reload on the
				// remote endpoint, these objects won't be synced again until the next reload.
				UpdateConfigObjectWithParents(object, azone, aclient, syncedObjects);
			}
		}
	}

	Log(LogInformation, "ApiListener")
		<< "Finished syncing runtime objects to endpoint '" << endpoint->GetName() << "'.";
}
