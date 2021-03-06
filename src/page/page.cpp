/**
############################################################################
# GPL License                                                              #
#                                                                          #
# This file is part of the JIM-DB.                                         #
# Copyright (c) 2015, Benjamin Meyer, <benjamin.meyer@tu-clausthal.de>     #
# This program is free software: you can redistribute it and/or modify     #
# it under the terms of the GNU General Public License as                  #
# published by the Free Software Foundation, either version 3 of the       #
# License, or (at your option) any later version.                          #
#                                                                          #
# This program is distributed in the hope that it will be useful,          #
# but WITHOUT ANY WARRANTY; without even the implied warranty of           #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            #
# GNU General Public License for more details.                             #
#                                                                          #
# You should have received a copy of the GNU General Public License        #
# along with this program. If not, see <http://www.gnu.org/licenses/>.     #
############################################################################
**/

#include "page.h"
#include "../assert.h"
#include "../meta/metadata.h"
#include "../meta/metaindex.h"
#include "../datatype/headermetadata.h"
#include "../index/objectindex.h"
#include "../datatype/arrayitem.h"
#include "../datatype/arrayitemstring.h"
#include "../datatype/arraytype.h"
#include "../common/configuration.h"

namespace jimdb
{
    namespace memorymanagement
    {
        std::atomic_ullong Page::m_s_idCounter(0);
        std::atomic_ullong Page::m_objCount(0);

        uint64_t GUID::id = 0;
        tasking::SpinLock GUID::m_lock;

        uint64_t GUID::get()
        {

            std::lock_guard<tasking::SpinLock> lock(m_lock);
            return ++id;
        }

        //take care order does matter here since header and body need to be init first to set the freepos values!
        Page::Page(long long header, long long body) : m_id(++m_s_idCounter), m_header(new char[header]),
            m_body(new char[body]),
            m_pageClean(common::Configuration::getInstance()[common::PAGE_FRAGMENTATION_CLEAN].GetDouble()),
            //set the freepos value ptr to the first header slot
            m_freeSpace(body),
            //set the header free pos ptr to the second  "long long" slot
            m_freepos(new(static_cast<char*>(m_header)) long long(0)),
            m_headerFreePos(new(static_cast<char*>(m_header) + sizeof(long long)) long long(0))
        {
            //because of m_freepos and m_headerFreePos
            m_headerSpace = header - 2 * sizeof(long long);

            //add the free type into the body
            m_free = new(&static_cast<char*>(m_body)[0]) FreeType(body);
            ASSERT(m_free->getFree() == body);

            //set the free of the header
            m_headerFree = new(static_cast<char*>(m_header) + 2 * sizeof(long long)) FreeType(header - 2 * sizeof(long long));
        }

        Page::~Page()
        {
            //clean up all data
            delete[] m_header;
            delete[] m_body;
        }

        long long Page::getID()
        {
            tasking::RWLockGuard<> lock(m_rwLock, tasking::READ);
            return m_id;
        }

        long long Page::free()
        {
            tasking::RWLockGuard<> lock(m_rwLock, tasking::READ);
            return m_freeSpace;
        }

        bool Page::hasHeaderSlot()
        {
            tasking::RWLockGuard<> lock(m_rwLock, tasking::READ);
            return findHeaderPosition(false) != nullptr;
        }

        bool Page::free(size_t size)
        {
            return find(size, false) != nullptr && findHeaderPosition(false) != nullptr;
        }

        size_t Page::insert(const rapidjson::GenericValue<rapidjson::UTF8<>>& value)
        {
            tasking::RWLockGuard<> lock(m_rwLock, tasking::WRITE);
            //here we know this page has one chunk big enough to fitt the whole object
            //including its overhead! so start inserting it.

            //get the object name
            std::string name = value.MemberBegin()->name.GetString();

            //returns the pos of the first element needed for the headerdata

            auto firstElementPos = insertObject(value.MemberBegin()->value, nullptr).first;
            auto l_first = dist(m_body, firstElementPos);

            //insert the header
            HeaderMetaData* meta = insertHeader(m_objCount++, 0, common::FNVHash()(name), l_first);
            if (meta == nullptr)
                LOG_DEBUG << "wm";
            //push the obj to obj index
            index::ObjectIndex::getInstance().add(meta->getOID(), index::ObjectIndexValue(this->m_id, dist(m_header, meta)));

            //done!
            return meta->getOID();
        }

        void Page::setObjCounter(long long value) const
        {
            m_objCount = value;
        }


        std::shared_ptr<rapidjson::Document> Page::getJSONObject(long long headerpos)
        {
            //reading the Page
            tasking::RWLockGuard<> lock(m_rwLock, tasking::READ);
            //get the header first
            auto l_header = reinterpret_cast<HeaderMetaData*>(static_cast<char*>(m_header) + headerpos);
            //get the type
            auto l_objectType = l_header->getObjektType();

            //get the idx
            auto& l_metaIdx = meta::MetaIndex::getInstance();
            //get the meta dataset
            auto& l_meta = l_metaIdx[l_objectType];

            //create the document
            auto l_obj = std::make_shared<rapidjson::Document>();
            l_obj->SetObject();

            //now generate the inner part of the object
            rapidjson::Value l_value;
            l_value.SetObject();
            //calc the start id
            void* start = (static_cast<char*>(m_body) + l_header->getPos());
            auto temp = buildObject(l_objectType, start, l_value, l_obj->GetAllocator());
            if(temp == nullptr)
            {
                LOG_WARN << "build failed id: " << l_header->getOID();
                return nullptr;
            }
            //generate name
            auto l_objName = l_meta->getName();
            rapidjson::Value l_name(l_objName.c_str(), l_objName.length(), l_obj->GetAllocator());
            //now add the inner object
            l_obj->AddMember(l_name, l_value, l_obj->GetAllocator());

            return l_obj;
        }

        bool Page::deleteObj(long long headerpos)
        {
            //reading the Page
            tasking::RWLockGuard<> lock(m_rwLock, tasking::WRITE);
            auto l_header = reinterpret_cast<HeaderMetaData*>(static_cast<char*>(m_header) + headerpos);
            auto l_oid = l_header->getOID();
            //start delete by calculating the start position and passing the hash of the obj name
            deleteObj(l_header->getObjektType(), static_cast<char*>(m_body) + l_header->getPos());

            //now its delete put the freetype ptr to the start
            //of the "prepared" deleted object
            auto l_free = m_free;
            while (l_free->getNext() == 0)
                l_free = reinterpret_cast<FreeType*>(reinterpret_cast<char*>(l_free) + l_free->getNext());
            // now l_Free is the last element;
            l_free->setNext(dist(l_free, static_cast<char*>(m_body) + l_header->getPos()));

            // Delete header
            l_free = m_headerFree;
            while (l_free->getNext() == 0)
                l_free = reinterpret_cast<FreeType*>(reinterpret_cast<char*>(l_free) + l_free->getNext());

            //now we have the last free element of the header
            //insert free type with the right size
            auto l_deletedHeader = new(l_header) FreeType(sizeof(HeaderMetaData));
            //set the freeType pointer to it
            l_free->setNext(dist(l_free, l_deletedHeader));

            //delete the obj of the idx
            index::ObjectIndex::getInstance().erase(l_oid);
            //do NOT delete the Meta! it is way faster to
            //insert with meta then without so keep the data
            return true;
        }


        /** short explaination
        - First we find a new FreeType where we can fit the Type.
        	- this automatically alocates the space and we can fit it
        	  without taking care of the freetype. Everything happens in free with aloc flag true
        - create the new Type at the position we got returned
        - update the previous next pointer to "us"
        - done
        */

        std::pair<void*, void*> Page::insertObject(const rapidjson::GenericValue<rapidjson::UTF8<>>& value,
                BaseType<size_t>* const last)
        {
            //return ptr to the first element
            void* l_ret = nullptr;
            //prev element ptr
            BaseType<size_t>* l_prev = last;

            //position pointer
            void* l_pos = nullptr;
            //get the members
            for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
            {
                switch (it->value.GetType())
                {
                    case rapidjson::kNullType:
                        LOG_WARN << "null type: " << it->name.GetString();
                        continue;

                    case rapidjson::kFalseType:
                    case rapidjson::kTrueType:
                        {
                            l_pos = find(sizeof(BoolTyp));

                            void* l_new = new (l_pos) BoolTyp(it->value.GetBool());

                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                            //set the ret pointer
                            if (l_ret == nullptr)
                                l_ret = l_pos;
                        }
                        break;
                    case rapidjson::kObjectType:
                        {
                            //pos for the obj id
                            //and insert the ID of the obj
                            l_pos = find(sizeof(ObjHashTyp));
                            std::string name = it->name.GetString();
                            auto hash = common::FNVHash()(name);
                            void* l_new = new (l_pos) ObjHashTyp(hash);



                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));

                            // pass the objid Object to the insertobj!
                            // now recursive insert the obj
                            // the second contains the last element inserted
                            // l_pos current contains the last inserted element and get set to the
                            // last element of the obj we insert
                            l_pos = insertObject(it->value, RC(SizeTType*, l_new)).second;

                            //set the ret pointer
                            if (l_ret == nullptr)
                                l_ret = l_new;
                        }
                        break;

                    case rapidjson::kArrayType:
                        {

                            //now insert values
                            l_pos = find(sizeof(ArrayType));

                            //insert the number of elements which follows
                            size_t l_size = it->value.Size();
                            void* l_new = new(l_pos) ArrayType(l_size);
                            //update prev
                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));

                            //insert elements
                            l_pos = insertArray(it->value, SC(SizeTType*, l_new));

                            //set the ret pointer
                            if (l_ret == nullptr)
                                l_ret = l_new;

                        }
                        break;

                    case rapidjson::kStringType:
                        {
                            // find pos where the string fits
                            // somehow we get here sometimes and it does not fit!
                            // which cant be since we lock the whole page
                            l_pos = find(sizeof(StringType) + it->value.GetStringLength());

                            //add the String Type at the pos of the FreeType
                            auto* l_new = new (l_pos) StringType(it->value.GetString());
                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                            //set the ret pointer
                            if (l_ret == nullptr)
                                l_ret = l_pos;
                        }
                        break;

                    case rapidjson::kNumberType:
                        {
                            //doesnt matter since long long and double are equal on
                            // x64
                            //find pos where the string fits
                            l_pos = find(sizeof(IntTyp));

                            void* l_new;
                            if (it->value.IsInt() || it->value.IsInt64())
                            {
                                //insert INT
                                l_new = new (l_pos) IntTyp(it->value.GetInt64());
                            }
                            else
                            {
                                //INSERT DOUBLE
                                l_new = new (l_pos) DoubleTyp(it->value.GetDouble());
                            }
                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));

                            //set the ret pointer
                            if (l_ret == nullptr)
                                l_ret = l_pos;
                        }
                        break;
                    default:
                        LOG_WARN << "Unknown member Type: " << it->name.GetString() << ":" << it->value.GetType();
                        continue;
                }
                //prev is the l_pos now so cast it to this;
                l_prev = RC(SizeTType*, l_pos);
            }
            //if we get here its in!
            return{ l_ret, l_pos };
        }

        void* Page::find(size_t size, bool aloc)
        {
            //we cant fit it
            if (size >= m_freeSpace)
                return nullptr;
            //now get the next free slot which could fit it
            FreeType* l_prev = nullptr;
            auto l_freePtr = m_free;
            FreeType* l_ret = nullptr;
            while(true)
            {
                //return the position we we sure can fit it
                //without losing any chunks
                if (l_freePtr->getFree() > size + sizeof(FreeType))
                {
                    l_ret = l_freePtr;//safe the spot
                    break;
                }

                if(l_freePtr->getFree() == size)
                {
                    //if it exactly fit we also take the spot
                    l_ret = l_freePtr;
                    break;
                }

                if (l_freePtr->getNext() != 0)
                {
                    //else it does not fit and we need to go to the next

                    l_prev = l_freePtr;//set the previous
                    auto l_temp = RC(char*, l_freePtr);
                    l_temp += l_freePtr->getNext();
                    l_freePtr = RC(FreeType*, l_temp);
                    continue;
                }
                //if we get here we have no space for that!
                return nullptr;
            }
            //now we are here and need to aloc
            if(aloc)
            {
                m_freeSpace -= size;

                //now check if we lose (cuz it fits) one or need to create a new freeType
                if (l_ret->getFree() == size)
                {
                    //l_ret will be "deleted" out of the chain
                    if (l_prev == nullptr)
                    {
                        //if prev is null we have the start
                        //so update the m_free ptr
                        m_free = RC(FreeType*, RC(char*, l_ret) + l_ret->getNext());
                    }
                    else
                    {
                        //set prev next to the distance
                        //of prev -> to next + the next of that
                        l_prev->setNext(dist(l_prev, l_ret) + l_ret->getNext());
                    }
                }
                else
                {

                    //we regular can fit the size and do not need to "delete" one
                    //throw in the "new" free at l_ret pos and create a new freetype "after" it
                    // with the same FreeSize - size

                    //store the next since we new on that position now!
                    auto l_next = l_ret->getNext();

                    //check if we had the head if so update it
                    if (l_prev == nullptr)
                    {
                        m_free = new(RC(char*, l_ret) + size) FreeType(l_ret->getFree() - size);
                        *m_freepos = dist(m_body, m_free);
                        m_free->setNext(dist(m_free, l_ret) + l_next);
                    }
                    else
                    {
                        auto l_newF = new(RC(char*, l_ret) + size) FreeType(l_ret->getFree() - size);
                        //next is relativ to the l_ret and of that the next soo...
                        l_newF->setNext(dist(l_newF, l_ret) + l_next);
                        //update the prev
                        l_prev->setNext(dist(l_prev, l_newF));
                    }
                }
            }
            return l_ret;
        }


        std::ptrdiff_t Page::dist(const void* const p1, const void* const p2) const
        {
            //dist from 1 to 2 is  2-1 ....
            return static_cast<const char*>(p2) - static_cast<const char*>(p1);
        }

        void* Page::insertArray(const rapidjson::GenericValue<rapidjson::UTF8<>>& arr, BaseType<size_t>* prev)
        {
            ArrayItem<size_t>* l_prev = reinterpret_cast<ArrayItem<size_t>*>(prev);
            for (auto arrayIt = arr.Begin(); arrayIt != arr.End(); ++arrayIt)
            {
                void* l_pos = nullptr;

                switch (arrayIt->GetType())
                {
                    case rapidjson::kNullType:
                        break;
                    case rapidjson::kFalseType:
                    case rapidjson::kTrueType:
                        {
                            //doesnt matter since long long and double are equal on
                            // x64
                            //find pos where the string fits
                            l_pos = find(sizeof(ArrayBoolTyp));

                            void* l_new = new (l_pos) ArrayBoolTyp(arrayIt->GetBool(), BOOL);

                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                        }
                        break;
                    case rapidjson::kObjectType:
                        break;
                    case rapidjson::kArrayType:
                        {
                            l_pos = find(sizeof(ArrayArrayCountTyp));
                            //add the number of elements
                            void* l_new = new(l_pos) ArrayArrayCountTyp(arrayIt->Size(), ARRAY);
                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                            l_pos = insertArray(*arrayIt, SC(SizeTType*, l_pos));
                        }
                        break;
                    case rapidjson::kStringType:
                        {

                            l_pos = find(sizeof(ArrayItemString) + arrayIt->GetStringLength());

                            void* l_new = new (l_pos) ArrayItemString(arrayIt->GetString());

                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                        }
                        break;
                    case rapidjson::kNumberType:
                        {
                            //doesnt matter since long long and double are equal on
                            // x64
                            //find pos where the string fits
                            l_pos = find(sizeof(ArrayIntTyp));

                            void* l_new;
                            if (arrayIt->IsInt())
                            {
                                //insert INT
                                l_new = new (l_pos) ArrayIntTyp(arrayIt->GetInt64(), INT);
                            }
                            else
                            {
                                //INSERT DOUBLE
                                l_new = new (l_pos)	ArrayDoubleTyp(arrayIt->GetDouble(), DOUBLE);
                            }

                            if (l_prev != nullptr)
                                l_prev->setNext(dist(l_prev, l_new));
                        }
                        break;
                    default:
                        break;
                }
                //update the prev
                l_prev = reinterpret_cast<ArrayItem<size_t>*>(l_pos);
            }
            return l_prev;
        }


        HeaderMetaData* Page::insertHeader(size_t id, size_t hash, size_t type, size_t pos)
        {
            auto l_meta = new(findHeaderPosition()) HeaderMetaData(id, hash, type, pos);
            return l_meta;
        }

        void* Page::findHeaderPosition(bool aloc)
        {
            //we cant fit it
            if (sizeof(HeaderMetaData) > m_headerSpace)
                return nullptr;
            //now get the next free slot which could fit it
            FreeType* l_prev = nullptr;
            auto l_freePtr = m_headerFree;
            FreeType* l_ret = nullptr;
            while (true)
            {
                //return the position we we sure can fit it
                //without losing any chunks
                if (l_freePtr->getFree() > sizeof(HeaderMetaData) + sizeof(FreeType))
                {
                    l_ret = l_freePtr;//safe the spot
                    break;
                }

                if (l_freePtr->getFree() == sizeof(HeaderMetaData))
                {
                    //if it exactly fit we also take the spot
                    l_ret = l_freePtr;
                    break;
                }

                if (l_freePtr->getNext() != 0)
                {
                    //else it does not fit and we need to go to the next

                    l_prev = l_freePtr;//set the previous
                    auto l_temp = reinterpret_cast<char*>(l_freePtr);
                    l_temp += l_freePtr->getNext();
                    l_freePtr = reinterpret_cast<FreeType*>(l_temp);
                    continue;
                }
                //if we get here we have no space for that!
                return nullptr;
            }

            if (aloc)
            {
                m_headerSpace -= sizeof(HeaderMetaData);

                //now check if we lose (cuz it fits) one or need to create a new freeType
                if (l_ret->getFree() == sizeof(HeaderMetaData))
                {
                    //l_ret will be "deleted" out of the chain
                    if (l_prev == nullptr)
                    {
                        //if prev is null we have the start
                        //so update the m_free ptr
                        m_headerFree = reinterpret_cast<FreeType*>(reinterpret_cast<char*>(l_ret) + l_ret->getNext());
                    }
                    else
                    {
                        //set prev next to the distance
                        //of prev -> to next + the next of that
                        l_prev->setNext(dist(l_prev, l_ret) + l_ret->getNext());
                    }
                }
                else
                {
                    //we regular can fit the size and do not need to "delete" one
                    //throw in the "new" free at l_ret pos and create a new freetype "after" it
                    // with the same FreeSize - size

                    //store the next since we new on that position now!
                    auto l_next = l_ret->getNext();

                    //check if we had the head if so update it
                    if (l_prev == nullptr)
                    {
                        //new freetype at that position with size - headermetadata
                        m_headerFree = new(reinterpret_cast<char*>(l_ret) + sizeof(HeaderMetaData))
                        FreeType(l_ret->getFree() - sizeof(HeaderMetaData));

                        *m_headerFreePos = dist(m_header, m_headerFree);
                        if(l_next != 0)
                            m_headerFree->setNext(dist(m_headerFree, l_ret) + l_next);
                    }
                    else
                    {
                        auto l_newF = new(reinterpret_cast<char*>(l_ret) + sizeof(HeaderMetaData))
                        FreeType(l_ret->getFree() - sizeof(HeaderMetaData));
                        //next is relativ to the l_ret and of that the next soo...
                        l_newF->setNext(dist(l_newF, l_ret) + l_next);
                        //update the prev
                        l_prev->setNext(dist(l_prev, l_newF));
                    }
                }
            }
            return l_ret;
        }

        void* Page::buildObject(size_t hash, void* start, rapidjson::Value& l_obj,
                                rapidjson::MemoryPoolAllocator<>& aloc)
        {
            //get the meta information of the object type
            //to build it
            auto& l_metaIdx = meta::MetaIndex::getInstance();

            //get the meta dataset
            auto& l_meta = l_metaIdx[hash];

            //now we are already in an object here with l_obj!
            auto l_ptr = start;
            for (auto it = l_meta->begin(); it != l_meta->end(); ++it)
            {
                //create the name value
                rapidjson::Value l_name(it->name.c_str(), it->name.length(), aloc);
                //create the value we are going to add
                rapidjson::Value l_value;
                //now start building it up again
                switch (it->type)
                {
                    case meta::OBJECT:
                        {
                            auto l_data = SC(ObjHashTyp*, l_ptr);
                            //get the hash to optain the metadata
                            auto l_hash = l_data->getData();
                            //set to object and create the inner object
                            l_value.SetObject();

                            //get the start pointer which is the "next" element
                            //and call recursive
                            l_ptr = SC(SizeTType*, buildObject(l_hash, RC(char*, l_data) + l_data->getNext(), l_value, aloc));
                        }
                        break;
                    case meta::ARRAY:
                        {
                            l_value.SetArray();
                            auto l_data = SC(ArrayType*, l_ptr);
                            //get the hash to optain the metadata
                            auto l_size = l_data->size();
                            l_ptr = buildArray(l_size, SC(char*, l_ptr) + l_data->getNext(), l_value, aloc);
                        }
                        break;
                    case meta::INT:
                        {
                            //create the data
                            auto l_data = SC(IntTyp*, l_ptr);
                            //with length attribute it's faster ;)
                            l_value = l_data->getData();
                            l_ptr = SC(char*, l_ptr) + SC(SizeTType*, l_ptr)->getNext();
                        }
                        break;
                    case meta::DOUBLE:
                        {
                            //create the data
                            auto l_data = SC(DoubleTyp*, l_ptr);
                            //with length attribute it's faster ;)
                            l_value = l_data->getData();
                            l_ptr = SC(char*, l_ptr) + SC(SizeTType*, l_ptr)->getNext();
                        }
                        break;
                    case meta::STRING:
                        {
                            //create the data
                            auto l_data = SC(StringType*, l_ptr);
                            //with length attribute it's faster
                            l_value.SetString(l_data->getString()->c_str(), l_data->getString()->length(), aloc);
                            l_ptr = SC(char*, l_ptr) + SC(SizeTType*, l_ptr)->getNext();
                        }
                        break;
                    case meta::BOOL:
                        {
                            //create the data
                            auto l_data = SC(BoolTyp*, l_ptr);
                            l_value = l_data->getData();
                            l_ptr = SC(char*, l_ptr) + SC(SizeTType*, l_ptr)->getNext();
                        }
                        break;
                    default:
                        break;
                }
                l_obj.AddMember(l_name, l_value, aloc);
                //update the lptr
            }
            //return the l_ptr which current shows to the next lement. //see line above
            return l_ptr;
        }

        void* Page::buildArray(long long elemCount, void* start, rapidjson::Value& toAdd,
                               rapidjson::MemoryPoolAllocator<>& aloc)
        {
            ArrayItem<size_t>* l_element = static_cast<ArrayItem<size_t>*>(start);
            // we know every element which follows is a Array Element
            // we also know that toAdd is already a array!
            rapidjson::Value l_value;
            for (auto i = 0; i < elemCount; ++i)
            {
                switch (l_element->getType())
                {
                    case INT:
                        {
                            l_value = reinterpret_cast<ArrayItem<long long>*>(l_element)->getData();
                            toAdd.PushBack(l_value, aloc);
                        }
                        break;
                    case BOOL:
                        {
                            l_value = reinterpret_cast<ArrayItem<bool>*>(l_element)->getData();
                            toAdd.PushBack(l_value, aloc);
                        }
                        break;
                    case OBJECT:
                        break;
                    case ARRAY:
                        {
                            l_value.SetArray();

                            //void* l_start = reinterpret_cast<char*>(l_element) + l_element->getNext();
                            //void* temp = buildArray(reinterpret_cast<ArrayType*>(l_element)->size(), l_start, l_value, aloc);
                            l_element = static_cast < ArrayItem<size_t>*>( buildArray(reinterpret_cast<ArrayItem<size_t>*>(l_element)->getData(),
                                        reinterpret_cast<char*>(l_element) + l_element->getNext(),
                                        l_value, aloc));

                            toAdd.PushBack(l_value, aloc);
                        }
                        break;
                    case DOUBLE:
                        {
                            l_value = reinterpret_cast<ArrayItem<double>*>(l_element)->getData();
                            toAdd.PushBack(l_value, aloc);
                        }
                        break;
                    case STRING:
                        {
                            auto l_data = reinterpret_cast<ArrayItemString*>(l_element)->getString();
                            l_value.SetString(l_data->c_str(), l_data->length(), aloc);
                            toAdd.PushBack(l_value, aloc);
                        }
                        break;
                    case INVALID:
                        break;
                    default:
                        break;
                }
                //update the lptr to the next element;
                l_element = reinterpret_cast<ArrayItem<size_t>*>(reinterpret_cast<char*>(l_element) + l_element->getNext());
            }
            return l_element;
        }


        void* Page::deleteObj(size_t hash, void* start)
        {
            //get the meta data
            auto& l_meta = meta::MetaIndex::getInstance()[hash];
            auto l_ptr = start;
            for (auto it = l_meta->begin(); it != l_meta->end(); ++it)
            {
                switch(it->type)
                {
                    case meta::OBJECT:
                        {
                            //for the header
                            m_freeSpace += sizeof(BaseType<size_t>);
                            //get the hash befor delete
                            auto l_hash = static_cast<BaseType<size_t>*>(l_ptr)->getData();
                            //now overrride the size
                            static_cast<BaseType<size_t>*>(l_ptr)->setData(0);
                            //recursive go forward
                            l_ptr = deleteObj(l_hash, static_cast<char*>(l_ptr) + static_cast<BaseType<size_t>*>(l_ptr)->getNext());
                        }
                        break;
                    case meta::ARRAY:
                        {
                            //TODO delete this array!
                        }
                        break;
                    case meta::STRING:
                        {
                            m_freeSpace += static_cast<FreeType*>(l_ptr)->getFree();
                            //nothing todo here since string = freetype
                            l_ptr = static_cast<char*>(l_ptr) + static_cast<BaseType<size_t>*>(l_ptr)->getNext();
                        }
                        break;
                    //in this cases we need todo always the same
                    case meta::INT:
                    case meta::DOUBLE:
                    case meta::BOOL:
                        {
                            m_freeSpace += sizeof(BaseType<size_t>);
                            //set the data to 0 so it is a string size 0
                            //which is a free type with size sizeof(FreeType)
                            static_cast<BaseType<size_t>*>(l_ptr)->setData(0);
                            l_ptr = static_cast<char*>(l_ptr) + static_cast<BaseType<size_t>*>(l_ptr)->getNext();
                        }
                        break;
                    default:
                        break;
                }
            }
            return l_ptr;
        }
    }
}