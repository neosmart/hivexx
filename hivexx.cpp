#include "hivexx.h"
#include <hivex.h>
#include <boost/format.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <nst-log/Log.h>

using namespace std;
using namespace hivexx;
using namespace neosmart;

Hive::HiveWrapper::HiveWrapper(hive_h *hive)
{
	_hive = hive;
}

Hive::HiveWrapper::~HiveWrapper()
{
	if (_hive != nullptr)
	{
		hivex_close(_hive);
	}
}

Hive::Hive(const std::string &path)
	: Node()
{
	Load(path);
}

bool Hive::Load(const std::string &path)
{
	_path = path;
	_cachedName = std::move(string(path.c_str() + path.find_last_of("/\\") + 1));

	//Test if it physically exists first
	if (FILE *file = fopen(_path.c_str(), "r"))
	{
		fclose(file);
		_hive = hivex_open(path.c_str(), HIVEX_OPEN_WRITE | HIVEX_OPEN_VERBOSE);
    }

	_hiveWrapper = make_shared<HiveWrapper>(_hive);
	if (_hive != nullptr)
	{
		_node = hivex_root(_hive);
		return _node != 0;
	}

	return false;
}

Hive::~Hive()
{
}

bool Hive::Save()
{
	logger.Debug("Hive %s saved", Name().c_str());
	return hivex_commit(_hive, NULL, 0) == 0;
}

Key::Key(const Node &node, hive_value_h value, const std::string &name)
{
	_node = node._node;
	_hive = node._hive;
	_value = value;
	_cachedName = name;
	_cachedPath = str(boost::format("%s\\%s") % node.Name() % name);
}

Node::Node(hive_h *hive, hive_node_h node, const std::string &name)
{
	_hive = hive;
	_node = node;
	_cachedName = name;
}

const std::string &Hive::Path() const
{
	return _path;
}

const std::string &Node::Name() const
{
	return _cachedName;
}

bool Key::Exists() const
{
	//ScopeLog x(__FUNCTION__);
	return _value != 0;
}

bool Node::Exists() const
{
	//ScopeLog x(__FUNCTION__);
	return _node != 0 && _hive != nullptr;
}

template <typename T>
bool Key::ChangeIfNotEqualTo(T &&compare)
{
	if (!Exists())
	{
		logger.Debug("ChangeIfNotEqualTo: %s not found", _cachedName.c_str());
		return false;
	}

	T oldValue;
	if (GetValue(oldValue) && oldValue != compare)
	{
		auto logMessage = str(boost::format("ChangeIfNotEqual: changing %1% from %2% to %3%") % _cachedName % oldValue % compare);
		logger.Debug("%s", logMessage.c_str());
		return SetValue(compare);
	}

	return false;
}

template bool Key::ChangeIfNotEqualTo(int&&);
template bool Key::ChangeIfNotEqualTo(std::string&&);

Node Node::GetNode(const std::string &path, bool create)
{
	hive_node_h node = _node;
	bool done = false;
	char *stub, *pathBuffer;
	pathBuffer = strdup(path.c_str());
	bool notFound = false;

	for (char *letter = stub = pathBuffer; !done && node && *stub; ++letter)
	{
		if ((*letter == '\\') || (done = *letter == '\0'))
		{
			*letter = '\0';
			hive_node_h parentNode = node;
			node = hivex_node_get_child(_hive, node, stub);
			if (node == 0)
			{
				if (create)
				{
					node = hivex_node_add_child(_hive, parentNode, stub);
				}
				else
				{
					notFound = true;
				}
			}
			stub = letter + 1;
		}
	}

	free(pathBuffer);

	if (notFound)
	{
		logger.Debug("Node not found: %s", path.c_str());
		return std::move(Node(nullptr, 0, path));
	}

	return std::move(Node(_hive, node, path));
}

Node Node::GetNode(const std::string &path)
{
	return GetNode(path, false);
}

Node Node::CreateNode(const std::string &path)
{
	auto node = GetNode(path, true);
	logger.Debug("CreateNode: %s", path.c_str());
	return std::move(node);
}

bool Node::DeleteKey(const std::string &name)
{
	throw std::exception();
	//Currently, libhivex does not support deleting a single key
	//In the future, this function should get the contents of the current node, remove the key from the vector, delete the node, and re-add
}

bool Node::DeleteNode(const std::string &path)
{
	auto node = GetNode(path);
	if (!node.Exists())
	{
		return true;
	}

	return hivex_node_delete_child(_hive, node._node) == 0;
}

std::vector<Node> Node::GetNodes()
{
	std::vector<Node> children;

	for (auto node = hivex_node_children(_hive, _node); node && *node; ++node)
	{
		char *temp = hivex_node_name(_hive, *node);
		Node newNode(_hive, *node, str(boost::format("%s\\%s") % _cachedName % temp));
		free (temp);

		children.push_back(std::move(newNode));
	}

	return std::move(children);
}

bool Key::SetValue(int32_t value)
{
	hive_set_value newValue = { const_cast<char *>(_cachedName.data()), hive_t_REG_DWORD, sizeof(int), (char *) &value };
	logger.Debug("SetValue %s: %i", _cachedPath.c_str(), value);
	return hivex_node_set_value(_hive, _node, &newValue, 0);
}

#include <boost/locale.hpp>
bool Key::SetValue(std::string value)
{
	std::u16string wideValue = boost::locale::conv::utf_to_utf<char16_t>(value);
	hive_set_value newValue = { const_cast<char *>(_cachedName.data()), hive_t_REG_SZ, (wideValue.size() + 1) * sizeof(char16_t), const_cast<char *>(reinterpret_cast<const char *>(wideValue.data())) };
	logger.Debug("SetValue %s: %s", _cachedPath.c_str(), value.c_str());
	return hivex_node_set_value(_hive, _node, &newValue, 0);
}

bool Key::GetValue(int32_t &result)
{
	hive_type type;
	size_t size = 0;
	if (Exists() && hivex_value_type(_hive, _value, &type, &size) == 0 && type == hive_t_REG_DWORD)
	{
		result = hivex_value_dword(_hive, _value);
		logger.Debug("GetValue %s: %i", _cachedPath.c_str(), result);
		return true;
	}
	logger.Debug("GetValue %s not found", _cachedPath.c_str());

	return false;
}

bool Key::GetValue(std::string &result)
{
	hive_type type;
	size_t size = 0;
	if (Exists() && hivex_value_type(_hive, _value, &type, &size) == 0 && type == hive_t_REG_SZ)
	{
		char *contents = hivex_value_string(_hive, _value);
		if (contents != nullptr)
		{
			result = contents;
			free(contents);
			logger.Debug("GetValue %s: %s", _cachedPath.c_str(), result.c_str());
			return true;
		}
	}
	logger.Debug("GetValue %s not found", _cachedPath.c_str());

	return false;
}

bool Node::Delete()
{
	logger.Debug("Delete: %s", _cachedName.c_str());
	return hivex_node_delete_child(_hive, _node) == 0;
}

Key Node::GetKey(const std::string &name)
{
	Key key(*this, hivex_node_get_value(_hive, _node, name.c_str()), name);
	return std::move(key);
}

std::vector<Key> Node::GetKeys()
{
	std::vector<Key> subkeys;

	hive_value_h *values = hivex_node_values(_hive, _node);
	for (auto &value = values; value != nullptr && *value != 0; ++value)
	{
		char *temp = hivex_value_key(_hive, *value);
		Key key(*this, *value, temp);
		free (temp);
		subkeys.push_back(std::move(key));
	}
	if (values != nullptr)
	{
		free(values);
	}

	return std::move(subkeys);
}

bool Node::SetKeys(const std::vector<Key> &keys)
{
	bool success = true;
	for (const auto &key : keys)
	{
		hive_set_value newValue = {0};
		newValue.value = hivex_value_value(key._hive, key._value, &newValue.t, &newValue.len);
		newValue.key = (char *) key._cachedName.c_str();
		if (newValue.t == hive_t_REG_SZ)
		{
			logger.Debug("SetKeys: %s set to %s", key._cachedPath.c_str(), newValue.value);
		}
		else if (newValue.t == hive_t_REG_DWORD)
		{
			logger.Debug("SetKeys: %s set to %s", key._cachedPath.c_str(), *(int32_t*)(void*)newValue.value);
		}
		success = success && hivex_node_set_value(_hive, _node, &newValue, 0);
		free(newValue.value);
	}

	return success;
}
