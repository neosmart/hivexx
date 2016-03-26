#include "hivexx.h"
#include <hivex.h>
#include <algorithm>
#include <boost/format.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <nst-log/Log.h>

using namespace std;
using namespace hivexx;
using namespace neosmart;

template<typename T>
cunique<T> make_cunique(T *t)
{
	static_assert(!is_void<T>::value, "unique_ptr of void is not valid!");
	return cunique<T>(std::forward<T*>(t), [] (T *t2) { if (t2 != nullptr) free(t2); });
}

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

//Hive::Hive(const std::string &path)
//	: Key()
//{
	//This constructor has been deprecated
	//It is very important to handle the return errors of Load()

	//Load(path);
//}

bool Hive::Load(const std::string &path)
{
	logger.SetLogLevel(Debug);
	_path = path;
	_cachedName = string(path.c_str() + path.find_last_of("/\\") + 1);

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

Key::Key(hive_h *hive, hive_node_h node, const std::string &name)
{
	_hive = hive;
	_node = node;
	_cachedName = name;
}

const std::string &Hive::Path() const
{
	return _path;
}

const std::string &Key::Name() const
{
	return _cachedName;
}

bool Key::Exists() const
{
	//ScopeLog x(__FUNCTION__);
	return _node != 0 && _hive != nullptr;
}

template <typename T>
bool Key::ChangeIfNotEqualTo(const string &name, T &&compare)
{
	if (!Exists())
	{
		logger.Warn("ChangeIfNotEqualTo: parent key %s not found", _cachedName.c_str());
		return false;
	}

	T oldValue;
	if (GetValue(name, oldValue) && oldValue != compare)
	{
		auto logMessage = str(boost::format("ChangeIfNotEqual: changing %1%\\%2% from %3% to %4%") % _cachedName % name.c_str() % oldValue % compare);
		logger.Debug("%s", logMessage.c_str());
		return SetValue(name, compare);
	}

	logger.Warn("ChangeIfNotEqualTo: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

template bool Key::ChangeIfNotEqualTo(const std::string &name, int&&);
template bool Key::ChangeIfNotEqualTo(const std::string &name, std::string&&);

Key Key::GetSubkey(std::string path, bool create)
{
	if (!Exists())
	{
		logger.Warn("GetSubkey: parent key %s not found", _cachedName.c_str());
		return Key();
	}

	logger.Debug("GetSubkey: %s", path.c_str());

	hive_node_h node = _node;
	bool done = false;
	char *stub;
	bool notFound = false;

	for (char *letter = stub = const_cast<char*>(path.data()); !done && node && *stub; ++letter)
	{
		if ((*letter == '\\') || (done = *letter == '\0'))
		{
			*letter = '\0';
			hive_node_h parentKey = node;
			node = hivex_node_get_child(_hive, node, stub);
			if (node == 0)
			{
				if (create)
				{
					logger.Debug("CreateSubkey: %s", stub);
					node = hivex_node_add_child(_hive, parentKey, stub);
				}
				else
				{
					notFound = true;
				}
			}
			stub = letter + 1;
		}
	}

	if (notFound)
	{
		logger.Debug("Key not found: %s", path.c_str());
		return Key(nullptr, 0, path);
	}

	return Key(_hive, node, path);
}

Key Key::CreateSubkey(const std::string &path)
{
	return GetSubkey(path, true);
}

//Returns true even if the value never existed. False only on failure to delete.
bool Key::DeleteValue(const std::string &name)
{
	if (!Exists())
	{
		logger.Warn("DeleteValue: parent key %s not found", _cachedName.c_str());
		return true; //highly-controversial, but pragmatically correct
	}

	//libhivex is non-destructive and append-only by nature
	//as such, it does not allow removal of a key, only changing its value
	//by pointing it a new value appended to the registry hive
	//We work around this by exporting the key contents, removing the entire key,
	//then re-importing the conutents minus the value we wish to remove
	auto values = GetValues();

	//We can use remove_if, but for efficiency's sake we should stop after one match
	for (auto value = values.cbegin(); value != values.cend(); ++value)
	{
		if (boost::iequals(name, value->Name.get()))
		{
			values.erase(value);
			return SetValues(values);
		}
	}

	//Value not found, but that's ok
	return true;
}

bool Key::DeleteSubkey(const std::string &path)
{
	if (!Exists())
	{
		logger.Warn("DeleteSubkey: parent key %s not found", _cachedName.c_str());
		return true; //highly-controversial, but pragmatically correct
	}

	auto node = GetSubkey(path, false);
	if (!node.Exists())
	{
		return true;
	}

	return hivex_node_delete_child(_hive, node._node) == 0;
}

std::vector<Key> Key::GetSubkeys()
{
	std::vector<Key> children;

	if (!Exists())
	{
		logger.Warn("GetSubkeys: parent key %s not found", _cachedName.c_str());
		return children;
	}

	for (auto node = hivex_node_children(_hive, _node); node && *node; ++node)
	{
		auto temp = make_cunique(hivex_node_name(_hive, *node));
		Key newKey(_hive, *node, str(boost::format("%s\\%s") % _cachedName % temp.get()));

		children.push_back(std::move(newKey));
	}

	return children;
}

bool Key::SetValue(const std::string &name, int32_t value)
{
	if (!Exists())
	{
		logger.Warn("SetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_set_value newValue = { const_cast<char *>(name.data()), hive_t_REG_DWORD, sizeof(int), (char *) &value };

	logger.Debug("SetValue %s\\%s: %i", _cachedName.c_str(), name.c_str(), value);
	return hivex_node_set_value(_hive, _node, &newValue, 0) == 0;
}

#include <boost/locale.hpp>
bool Key::SetValue(const std::string &name, std::string value)
{
	if (!Exists())
	{
		logger.Warn("SetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	auto wideValue = boost::locale::conv::utf_to_utf<char16_t>(value);
	hive_set_value newValue = { const_cast<char *>(name.data()), hive_t_REG_SZ, (wideValue.size() + 1) * sizeof(char16_t), const_cast<char *>(reinterpret_cast<const char *>(wideValue.data())) };
	logger.Debug("SetValue %s\\%s: %s", _cachedName.c_str(), name.c_str(), value.c_str());
	return hivex_node_set_value(_hive, _node, &newValue, 0) == 0;
}

bool Key::HasValue(const string &name) const
{
	if (!Exists())
	{
		logger.Warn("HasValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	return hivex_node_get_value(_hive, _node, name.c_str()) == 0;
}

bool Key::GetValue(const std::string &name, int32_t &result)
{
	if (!Exists())
	{
		logger.Warn("GetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_type type;
	size_t size = 0;
	auto value = hivex_node_get_value(_hive, _node, name.c_str());
	if (value != 0 && hivex_value_type(_hive, value, &type, &size) == 0 && type == hive_t_REG_DWORD)
	{
		result = hivex_value_dword(_hive, value);
		logger.Debug("GetValue %s\\%s: %d", _cachedName.c_str(), name.c_str(), result);
		return true;
	}
	
	logger.Debug("GetValue: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::GetValue(const std::string &name, std::string &result)
{
	if (!Exists())
	{
		logger.Warn("GetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_type type;
	size_t size = 0;
	auto value = hivex_node_get_value(_hive, _node, name.c_str());
	if (value != 0 && hivex_value_type(_hive, value, &type, &size) == 0 && type == hive_t_REG_SZ)
	{
		auto contents = make_cunique(hivex_value_string(_hive, value));
		if (contents != nullptr)
		{
			result = contents.get();
			logger.Debug("GetValue %s\\%s: %s", _cachedName.c_str(), name.c_str(), result.c_str());
			return true;
		}
	}

	logger.Debug("GetValue: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::Delete()
{
	logger.Debug("Delete: %s", _cachedName.c_str());
	return hivex_node_delete_child(_hive, _node) == 0;
}

vector<UntypedRegistryValue> Key::GetValues()
{
	vector<UntypedRegistryValue> values;

	if (!Exists())
	{
		logger.Warn("GetValues: key %s does not exist!", _cachedName.c_str());
		return values;
	}

	auto hValues = make_cunique(hivex_node_values(_hive, _node));
	if (!hValues)
	{
		logger.Error("hivex_node_values returned null! %d: %s", errno, strerror(errno));
		return values;
	}

	for (auto value = hValues.get(); value != nullptr && *value != 0; ++value)
	{
		UntypedRegistryValue val;
		val.Name = make_cunique(hivex_value_key(_hive, *value));
		val.Value = make_cunique(hivex_value_value(_hive, *value, &val.Type, &val.Length));

		values.push_back(std::move(val));
	}

	return values;
}

bool Key::SetValues(const std::vector<UntypedRegistryValue> &values)
{
	if (!Exists())
	{
		logger.Warn("SetValues: key %s does not exist!", _cachedName.c_str());
		return false;
	}

	bool success = true;
	for (const auto &value : values)
	{
		hive_set_value newValue;
		newValue.key = value.Name.get();
		newValue.t = value.Type;
		newValue.len = value.Length;
		newValue.value = value.Value.get();

		if (hivex_node_set_value(_hive, _node, &newValue, 0) != 0)
		{
			logger.Error("SetValues: hivex_node_set_value error! %d: %s", errno, strerror(errno));
			success = false;
		}
	}

	return success;
}
