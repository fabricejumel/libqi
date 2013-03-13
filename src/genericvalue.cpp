/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/

#include <boost/lexical_cast.hpp>

#include <qitype/genericvalue.hpp>
#include <qitype/genericobject.hpp>

qiLogCategory("qitype.genericvalue");

namespace qi
{

  std::pair<GenericValuePtr, bool> GenericValuePtr::convert(Type* targetType) const
  {
    // qiLogDebug() << "convert " << type->infoString() << ' ' << targetType->infoString();
    /* Can have false-negative (same effective type, different Type instances
     * but we do not care, correct check (by comparing info() result
     * is more expensive than the dummy conversion that will happen.
     */
    if (type == targetType)
    {
      return std::make_pair(*this, false);
    }

    if (!targetType || !type) {
      qiLogWarning() << "Conversion error: can't convert to/from a null type.";
      return std::make_pair(GenericValuePtr(), false);
    }

    GenericValuePtr result;
    Type::Kind skind = type->kind();
    Type::Kind dkind = targetType->kind();
    if (skind == dkind)
    {
      switch(skind)
      {
        case Type::Float:
          result.type = targetType;
          result.value = targetType->initializeStorage();
          static_cast<TypeFloat*>(targetType)->set(&result.value,
                                                   static_cast<TypeFloat*>(type)->get(value));
          return std::make_pair(result, true);
        case Type::Int:
          {
          TypeInt* tsrc = static_cast<TypeInt*>(type);
          TypeInt* tdst = static_cast<TypeInt*>(targetType);
          int64_t v = tsrc->get(value);
          /* Bounce to GVP to perform overflow checks
          */
          GenericValuePtr result((Type*)tdst);
          if (tsrc->isSigned())
            result.setInt(v);
          else
            result.setUInt((uint64_t)v);
          tdst->set(&result.value, v);
          return std::make_pair(result, true);
          }
        case Type::String:
          {
            if (targetType->info() == type->info())
              return std::make_pair(*this, false);
            result.type = targetType;
            result.value = targetType->initializeStorage();
            std::pair<char*, size_t> v = static_cast<TypeString*>(type)->get(value);
            static_cast<TypeString*>(targetType)->set(&result.value,
                                                      v.first, v.second);
            return std::make_pair(result, true);
          }
        case Type::List:
        {
          TypeList* targetListType = static_cast<TypeList*>(targetType);
          TypeList* sourceListType = static_cast<TypeList*>(type);

          Type* srcElemType = sourceListType->elementType();
          Type* dstElemType = targetListType->elementType();
          bool needConvert = (srcElemType->info() != dstElemType->info());
          result = GenericValuePtr((Type*)targetListType);

          GenericIterator iend = end();
          for (GenericIterator it = begin(); it!= iend; ++it)
          {
            GenericValuePtr val = *it;
            if (!needConvert)
              result._append(val);
            else
            {
              std::pair<GenericValuePtr,bool> c = val.convert(dstElemType);
              result._append(c.first);
              if (c.second)
                c.first.destroy();
            }
          }
          return std::make_pair(result, true);
        }
          break;
        case Type::Map:
        {
          result = GenericValuePtr(targetType);

          TypeMap* targetMapType = static_cast<TypeMap*>(targetType);
          TypeMap* srcMapType = static_cast<TypeMap*>(type);

          Type* srcKeyType = srcMapType->keyType();
          Type* srcElementType = srcMapType->elementType();

          Type* targetKeyType = targetMapType->keyType();
          Type* targetElementType = targetMapType->elementType();

          bool sameKey = srcKeyType->info() == targetKeyType->info();
          bool sameElem = srcElementType->info() == targetElementType->info();

          GenericIterator iend = end();
          for (GenericIterator it = begin(); it != iend; ++it)
          {
            std::pair<GenericValuePtr, bool> ck, cv;
            GenericValuePtr kv = *it;
            if (!sameKey)
            {
              ck = kv[0].convert(targetKeyType);
              if (!ck.first.type)
                return std::make_pair(GenericValuePtr(), false);
            }
            if (!sameElem)
            {
              cv = kv[1].convert(targetElementType);
              if (!cv.first.type)
                return std::make_pair(GenericValuePtr(), false);
            }
            result._insert(sameKey?kv[0]:ck.first, sameElem?kv[1]:cv.first);
            if (!sameKey && ck.second)
              ck.first.destroy();
            if (!sameElem && cv.second)
              cv.first.destroy();
          }
          return std::make_pair(result, true);
        }
          break;
        case Type::Pointer:
        {
          Type* srcPointedType = static_cast<TypePointer*>(type)->pointedType();
          Type* dstPointedType = static_cast<TypePointer*>(targetType)->pointedType();
          // We only try to handle conversion for pointer to objects
          if (srcPointedType->kind() != Type::Object || dstPointedType->kind() != Type::Object)
          {
            // However, we need the full check for exact match here
            if (type->info() == targetType->info())
              return std::make_pair(*this, false);
            else
              return std::make_pair(GenericValuePtr(), false);
          }
          GenericValuePtr pointedSrc = static_cast<TypePointer*>(type)->dereference(value);
          std::pair<GenericValuePtr, bool> pointedDstPair = pointedSrc.convert(dstPointedType);
          if (!pointedDstPair.first.type)
            return std::make_pair(GenericValuePtr(), false);
          if (pointedDstPair.second)
            qiLogError() << "assertion error, allocated converted reference";
          // We must re-reference
          GenericValuePtr pointedDst = pointedDstPair.first;
          void* ptr = pointedDst.type->ptrFromStorage(&pointedDst.value);
          result.type = targetType;
          result.value = targetType->initializeStorage(&ptr);
          return std::make_pair(result, false);
        }
          break;
        case Type::Tuple:
        {
          TypeTuple* tsrc = static_cast<TypeTuple*>(type);
          TypeTuple* tdst = static_cast<TypeTuple*>(targetType);
          std::vector<void*> sourceData = tsrc->get(value);
          std::vector<Type*> srcTypes = tsrc->memberTypes();
          std::vector<Type*> dstTypes = tdst->memberTypes();
          if (dstTypes.size() != sourceData.size())
          {
            qiLogWarning() << "Conversion failure: tuple size mismatch";
            return std::make_pair(GenericValuePtr(), false);
          }

          std::vector<void*> targetData;
          std::vector<bool> mustDestroy;
          for (unsigned i=0; i<dstTypes.size(); ++i)
          {
            std::pair<GenericValuePtr, bool> conv = GenericValuePtr(srcTypes[i], sourceData[i]).convert(dstTypes[i]);
            if (!conv.first.type)
            {
              qiLogWarning() << "Conversion failure in tuple member between "
                                      << srcTypes[i]->infoString() << " and " << dstTypes[i]->infoString();
              return std::make_pair(GenericValuePtr(), false);
            }
            targetData.push_back(conv.first.value);
            mustDestroy.push_back(conv.second);
          }
          void* dst = tdst->initializeStorage();
          tdst->set(&dst, targetData);
          for (unsigned i=0; i<mustDestroy.size(); ++i)
          {
            if (mustDestroy[i])
              dstTypes[i]->destroy(targetData[i]);
          }
          result.type = targetType;
          result.value = dst;
          return std::make_pair(result, true);
        }
        case Type::Dynamic: {
          result.type  = targetType;
          result.value = targetType->initializeStorage();
          static_cast<TypeDynamic*>(targetType)->set(&result.value, *this);
          return std::make_pair(result, true);
        }
        case Type::Raw: {
          result.type = targetType;
          result.value = targetType->initializeStorage();
          qi::Buffer buf = static_cast<TypeRaw*>(type)->get(value);
          static_cast<TypeRaw*>(targetType)->set(&result.value, buf);
          return std::make_pair(result, true);
        }
        default:
          break;
      } // switch
    } // skind == dkind
    if (skind == Type::Float && dkind == Type::Int)
    {
      double v = static_cast<TypeFloat*>(type)->get(value);
      TypeInt* tdst = static_cast<TypeInt*>(targetType);
      GenericValuePtr result((Type*)tdst);
      // bounce to setDouble for overflow check
      result.setDouble(v);
      return std::make_pair(result, true);
    }
    else if (skind == Type::Int && dkind == Type::Float)
    {
      GenericValuePtr result(targetType);
      int64_t v = static_cast<TypeInt*>(type)->get(value);
      if (static_cast<TypeInt*>(type)->isSigned())
        result.setInt(v);
      else
        result.setUInt((uint64_t)v);
      return std::make_pair(result, true);
    }
    else if (skind == Type::String && dkind == Type::Raw)
    {
      qi::Buffer buf;
      std::pair<char*, size_t> data = static_cast<TypeString*>(type)->get(value);
      memcpy(buf.reserve(data.second), data.first, data.second);
      result.type = targetType;
      result.value = targetType->initializeStorage();
      static_cast<TypeRaw*>(result.type)->set(&result.value, buf);
      return std::make_pair(result, true);
    }
    else if (skind == Type::Raw && dkind == Type::String)
    {
      qiLogWarning() << "Conversion attempt from raw to string";
      return std::make_pair(GenericValuePtr(), false);
    }
    if (targetType->kind() == Type::Dynamic)
    {
      result.type = targetType;
      result.value = targetType->initializeStorage();
      static_cast<TypeDynamic*>(targetType)->set(&result.value, *this);
      return std::make_pair(result, true);
    }
    if (type->info() == typeOf<ObjectPtr>()->info()
      && targetType->kind() == Type::Pointer
    && static_cast<TypePointer*>(targetType)->pointedType()->kind() == Type::Object)
    { // Attempt specialized proxy conversion
      detail::ProxyGeneratorMap& map = detail::proxyGeneratorMap();
      detail::ProxyGeneratorMap::iterator it = map.find(
        static_cast<TypePointer*>(targetType)->pointedType()->info());
      if (it != map.end())
      {
        GenericValuePtr res = (it->second)(*(ObjectPtr*)value);
        return std::make_pair(res, true);
      }
    }
    if (type->kind() == Type::Dynamic)
    {
      GenericValuePtr gv = asDynamic();
      std::pair<GenericValuePtr, bool> result = gv.convert(targetType);
      return result;
    }

    if (skind == Type::Object && dkind == Type::Pointer)
    {
      std::pair<GenericValuePtr, bool> gv = convert(
        static_cast<TypePointer*>(targetType)->pointedType());
      if (!gv.first.type)
        return gv;
      // Re-pointerise it
      void* ptr = gv.first.type->ptrFromStorage(&gv.first.value);
      GenericValuePtr result;
      result.type = targetType;
      result.value = targetType->initializeStorage(&ptr);
      return std::make_pair(result, false);
    }
    if (skind == Type::Object)
    {
      // Try inheritance
      ObjectType* osrc = static_cast<ObjectType*>(type);
      qiLogDebug() << "inheritance check "
                            << osrc <<" " << (osrc?osrc->inherits(targetType):false);
      int inheritOffset = 0;
      if (osrc && (inheritOffset =  osrc->inherits(targetType)) != -1)
      {
        // We return a Value that point to the same data as this.
        result.type = targetType;
        result.value = (void*)((long)value + inheritOffset);
        return std::make_pair(result, false);
      }
    }
    if (type->info() == targetType->info())
    {
      return std::make_pair(*this, false);
    }

    return std::make_pair(GenericValuePtr(), false);
  }

  GenericValuePtr GenericValuePtr::convertCopy(Type* targetType) const
  {
    std::pair<GenericValuePtr, bool> res = convert(targetType);
    if (res.second)
      return res.first;
    else
      return res.first.clone();
  }

  bool operator< (const GenericValuePtr& a, const GenericValuePtr& b)
  {
    qiLogDebug() << "Compare " << a.type << ' ' << b.type;
    #define GET(v, t) static_cast<Type ## t *>(v.type)->get(v.value)
    if (!a.type)
      return b.type;
    if (!b.type)
      return false;
    /* < operator for char* does not do what we want, so force
    * usage of get() below for string types.
    */
    if ((a.type == b.type || a.type->info() == b.type->info())
      && a.type->kind() != Type::String)
    {
      qiLogDebug() << "Compare sametype " << a.type->infoString();
      return a.type->less(a.value, b.value);
    }
    // Comparing values of different types
    Type::Kind ka = a.type->kind();
    Type::Kind kb = b.type->kind();
    qiLogDebug() << "Compare " << ka << ' ' << kb;
    if (ka != kb)
    {
      if (ka == Type::Int && kb == Type::Float)
        return GET(a, Int) < GET(b, Float);
      else if (ka == Type::Float && kb == Type::Int)
        return GET(a, Float) < GET(b, Int);
      else
        return ka < kb; // Safer than comparing pointers
    }
    else switch(ka)
    {
    case Type::Void:
      return false;
    case Type::Int:
      return GET(a, Int) < GET(b, Int);
    case Type::Float:
      return GET(a, Float) < GET(b, Float);
    case Type::String:
      {
        std::pair<char*, size_t> ca, cb;
        ca = GET(a, String);
        cb = GET(b, String);
        bool res = ca.second == cb.second?
        (memcmp(ca.first, cb.first, ca.second) < 0) : (ca.second < cb.second);
        qiLogDebug() << "Compare " << ca.first << ' ' << cb.first << ' ' << res;
        return res;
      }
    case Type::List:
    case Type::Map: // omg, same code!
      {
        size_t la = a.size();
        size_t lb = b.size();
        if (la != lb)
          return la < lb;
        GenericIterator ita   = a.begin();
        GenericIterator enda = a.end();
        GenericIterator itb   = b.begin();
        GenericIterator endb = b.end();
        while (ita != enda)
        {
          assert (! (itb == endb));
          GenericValuePtr ea = *ita;
          GenericValuePtr eb = *itb;
          if (ea < eb)
            return true;
          else if (eb < ea)
            return false;
          ++ita;
          ++itb;
        }
        return false; // list are equals
      }
    case Type::Object:
    case Type::Pointer:
    case Type::Tuple:
    case Type::Dynamic:
    case Type::Raw:
    case Type::Unknown:
    case Type::Iterator:
      return a.value < b.value;
    }
    #undef GET
    return a.value < b.value;
  }
  bool operator< (const GenericValue& a, const GenericValue& b)
  {
    return (const GenericValuePtr&)a < (const GenericValuePtr&)b;
  }

  bool operator==(const GenericValuePtr& a, const GenericValuePtr& b)
  {
    if (a.kind() == Type::Iterator && b.kind() == Type::Iterator
      && a.type->info() == b.type->info())
    {
      return static_cast<TypeIterator*>(a.type)->equals(a.value, b.value);
    }
    else
      return ! (a < b) && !(b<a);
  }

  bool operator==(const GenericValue& a, const GenericValue& b)
  {
    return (const GenericValuePtr&)a == (const GenericValuePtr&)b;
  }

  bool operator==(const GenericIterator& a, const GenericIterator& b)
  {
    return (const GenericValuePtr&)a == (const GenericValuePtr&)b;
  }

  GenericValue GenericValuePtr::toTuple(bool homogeneous) const
  {
    if (kind() == Type::Tuple)
      return GenericValue(*this);
    else if (kind() != Type::List)
      throw std::runtime_error("Expected Tuple or List kind");
    // convert list to tuple

    TypeList* t = static_cast<TypeList*>(type);
    Type* te = t->elementType();
    TypeDynamic* td = 0;
    if (te->kind() == Type::Dynamic)
      td = static_cast<TypeDynamic*>(te);
    if (!homogeneous && !td)
      throw std::runtime_error("Element type is not dynamic");
    std::vector<GenericValuePtr> elems;
    GenericIterator it = begin();
    GenericIterator iend = end();
    while (it != iend)
    {
      GenericValuePtr e = *it;
      if (homogeneous)
        elems.push_back(e);
      else
        elems.push_back(e.asDynamic());
      ++it;
    }

    //makeGenericTuple allocates, steal the result
    return GenericValue(makeGenericTuple(elems), false, true);
  }

  ObjectPtr GenericValuePtr::toObject() const
  {
    return to<ObjectPtr>();
  }

  GenericValuePtr GenericValuePtr::_element(const GenericValuePtr& key, bool throwOnFailure)
  {
    if (kind() == Type::List)
    {
      TypeList* t = static_cast<TypeList*>(type);
      int ikey = key.toInt();
      if (ikey < 0 || static_cast<size_t>(ikey) >= t->size(value))
      {
        if (throwOnFailure)
          throw std::runtime_error("Index out of range");
        else
          return GenericValuePtr();
      }
      return GenericValuePtr(t->elementType(), t->element(value, ikey));
    }
    else if (kind() == Type::Map)
    {
      TypeMap* t = static_cast<TypeMap*>(type);
      std::pair<GenericValuePtr, bool> c = key.convert(t->keyType());
      if (!c.first.type)
        throw std::runtime_error("Incompatible key type");
      // HACK: should be two separate booleans
      bool autoInsert = throwOnFailure;
      GenericValuePtr result
        = t->element(&value, c.first.value, autoInsert);
      if (c.second)
        c.first.destroy();
      return result;
    }
    else if (kind() == Type::Tuple)
    {
      TypeTuple* t = static_cast<TypeTuple*>(type);
      int ikey = key.toInt();
      std::vector<Type*> types = t->memberTypes();
      if (ikey < 0 || static_cast<size_t>(ikey) >= types.size())
      {
        if (throwOnFailure)
          throw std::runtime_error("Index out of range");
        else
          return GenericValuePtr();
      }
      return GenericValuePtr(types[ikey], t->get(value, ikey));
    }
    else
      throw std::runtime_error("Expected List, Map or Tuple kind");
  }

  void GenericValuePtr::_append(const GenericValuePtr& elem)
  {
    if (kind() != Type::List)
      throw std::runtime_error("Expected a list");
    TypeList* t = static_cast<TypeList*>(type);
    std::pair<GenericValuePtr, bool> c = elem.convert(t->elementType());
    t->pushBack(&value, c.first.value);
    if (c.second)
      c.first.destroy();
  }

  void GenericValuePtr::_insert(const GenericValuePtr& key, const GenericValuePtr& val)
  {
    if (kind() != Type::Map)
      throw std::runtime_error("Expected a map");
    std::pair<GenericValuePtr, bool> ck(key, false);
    std::pair<GenericValuePtr, bool> cv(val, false);
    TypeMap* t = static_cast<TypeMap*>(type);
    if (key.type != t->keyType())
      ck = key.convert(t->keyType());
    if (val.type != t->elementType())
      cv = val.convert(t->elementType());
    t->insert(&value, ck.first.value, cv.first.value);
    if (ck.second)
      ck.first.destroy();
    if (cv.second)
      cv.first.destroy();
  }

  void GenericValuePtr::update(const GenericValuePtr& val)
  {
    switch(kind())
    {
    case Type::Int:
      setInt(val.toInt());
      break;
    case Type::Float:
      setDouble(val.toDouble());
      break;
    case Type::String:
      setString(val.toString());
      break;
    default:
      throw std::runtime_error("Update not implemented for this type.");
    }
  }
}

