#pragma once
/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/

#ifndef _QIMESSAGING_DETAILS_FUNCTIONTYPE_HXX_
#define _QIMESSAGING_DETAILS_FUNCTIONTYPE_HXX_

#include <boost/fusion/include/mpl.hpp>
#include <boost/mpl/for_each.hpp>
#include <boost/mpl/transform_view.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/type_traits/add_pointer.hpp>
#include <boost/type_traits/remove_const.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/function_types/function_type.hpp>
#include <boost/function_types/result_type.hpp>
#include <boost/function_types/parameter_types.hpp>
#include <boost/fusion/container/vector/convert.hpp>
#include <boost/fusion/include/as_vector.hpp>
#include <boost/fusion/include/as_list.hpp>
#include <boost/fusion/algorithm/transformation/transform.hpp>
#include <boost/fusion/include/transform.hpp>
#include <boost/fusion/functional/invocation/invoke_function_object.hpp>
#include <boost/fusion/container/generation/make_vector.hpp>
#include <boost/fusion/include/make_vector.hpp>
#include <boost/fusion/algorithm/iteration/for_each.hpp>
#include <boost/fusion/functional/adapter/unfused.hpp>
#include <boost/fusion/functional/generation/make_unfused.hpp>
#include <boost/fusion/functional/generation/make_fused.hpp>
#include <qimessaging/genericvalue.hpp>

namespace qi
{
  inline Type* FunctionType::resultType()
  {
    return _resultType;
  }

  inline const std::vector<Type*>& FunctionType::argumentsType()
  {
    return _argumentsType;
  }

  namespace detail
  {
    struct PtrToConstRef
    {
      // Drop the const, it prevents method calls from working
      template <typename Sig>
      struct result;

      template <class Self, typename T>
      struct result< Self(T) >
      {
        typedef typename boost::add_reference<
        //typename boost::add_const<
        typename boost::remove_pointer<
        typename boost::remove_reference<T>::type
        >::type
        //  >::type
        >::type type;
      };
      template<typename T>
      T& operator() (T* ptr) const
      {
        return *ptr;
      }
    };
    template<typename T> struct remove_constptr
    {
      typedef T type;
    };
    template<typename T> struct remove_constptr<const T*>
    {
      typedef T* type;
    };
    struct fill_arguments
    {
      inline fill_arguments(std::vector<Type*>* target)
      : target(target) {}

      template<typename T> void operator()(T*) const
      {
        target->push_back(typeOf<
          typename remove_constptr<
            typename boost::remove_const<
               typename boost::remove_reference<T>::type
            >::type>::type>());
      }
      std::vector<Type*>* target;
    };

    struct Transformer
    {
    public:
      inline Transformer(const std::vector<void*>* args)
      : args(args)
      , pos(0)
      {}
      template <typename Sig>
      struct result;

      template <class Self, typename T>
      struct result< Self(T) >
      {
        typedef T type;
      };
      template<typename T>
      void
      operator() (T* &v) const
      {
        v = (T*)(*args)[pos++];
      }
      const std::vector<void*> *args;
      mutable unsigned int pos;
    };

    template<typename SEQ, typename F> void* apply(SEQ sequence,
      F& function, const std::vector<void*> args)
    {
      GenericValueCopy res;
      boost::fusion::for_each(sequence, Transformer(&args));
      res(), boost::fusion::invoke_function_object(function,
        boost::fusion::transform(sequence,
          PtrToConstRef()));
    return res.value;
    }
  } // namespace detail


  template<typename T> class FunctionTypeImpl:
  public virtual FunctionType,
  public virtual TypeImpl<boost::function<T> >
  {
  public:
    FunctionTypeImpl()
    {
      _resultType = typeOf<typename boost::function_types::result_type<T>::type >();
      typedef typename boost::function_types::parameter_types<T>::type ArgsType;
      boost::mpl::for_each<
        boost::mpl::transform_view<ArgsType,
        boost::add_pointer<
        boost::remove_const<
        boost::remove_reference<boost::mpl::_1> > > > >(detail::fill_arguments(&_argumentsType));
    }
    virtual void* call(void* func, const std::vector<void*>& args)
    {
      boost::function<T>* f = (boost::function<T>*)func;
      typedef typename boost::function_types::parameter_types<T>::type ArgsType;
      typedef typename  boost::mpl::transform_view<ArgsType,
      boost::remove_const<
      boost::remove_reference<boost::mpl::_1> > >::type BareArgsType;
      typedef typename boost::mpl::transform_view<BareArgsType,
      boost::add_pointer<boost::mpl::_1> >::type PtrArgsType;
      return detail::apply(boost::fusion::as_vector(PtrArgsType()), *f, args);
    }
  };

  template<typename T> FunctionType* makeFunctionType()
  {
    static FunctionTypeImpl<T> result;
    return &result;
  }

  template<typename T>
  GenericFunction makeGenericFunction(boost::function<T> f)
  {
    GenericFunction res;
    res.value = new boost::function<T>(f);
    res.type = makeFunctionType<T>();
    return res;
  }

  template<typename F> GenericFunction makeGenericFunction(F func)
  {
  return makeGenericFunction(boost::function<
    typename boost::remove_pointer<F>::type>(func));
  }


namespace detail
{
  /* Call a boost::function<F> binding the first argument.
  * Can't be done just with boost::bind without code generation.
  */
  template<typename F>
  struct FusedBindOne
  {
    template <class Seq>
    struct result
    {
      typedef typename boost::function_types::result_type<F>::type type;
    };

    template <class Seq>
    typename result<Seq>::type
    operator()(Seq const & s) const
    {
      return ::boost::fusion::invoke_function_object(func,
        ::boost::fusion::push_front(s, boost::ref(const_cast<ArgType&>(*arg1))));
    }
    ::boost::function<F> func;
    typedef typename boost::remove_reference<
      typename ::boost::mpl::front<
        typename ::boost::function_types::parameter_types<F>::type
        >::type>::type ArgType;
    void setArg(ArgType* val) { arg1 = val;}
    ArgType* arg1;

  };

}

template<typename C, typename F> GenericFunction makeGenericFunction(C* inst, F func)
{
  // Return type
  typedef typename ::boost::function_types::result_type<F>::type RetType;
  // All arguments including class pointer
  typedef typename ::boost::function_types::parameter_types<F>::type MemArgsType;
  // Pop class pointer
  typedef typename ::boost::mpl::pop_front< MemArgsType >::type ArgsType;
  // Synthethise exposed function type
  typedef typename ::boost::mpl::push_front<ArgsType, RetType>::type ResultMPLType;
  typedef typename ::boost::function_types::function_type<ResultMPLType>::type ResultType;
  // Synthethise non-member function equivalent type of F
  typedef typename ::boost::mpl::push_front<MemArgsType, RetType>::type MemMPLType;
  typedef typename ::boost::function_types::function_type<MemMPLType>::type LinearizedType;
  // See func as R (C*, OTHER_ARGS)
  boost::function<LinearizedType> memberFunction = func;
  boost::function<ResultType> res;
  // Create the fusor
  detail::FusedBindOne<LinearizedType> fusor;
  // Bind member function and instance
  fusor.setArg(inst);
  fusor.func = memberFunction;
  // Convert it to a boost::function
  res = boost::fusion::make_unfused(fusor);

  return makeGenericFunction(res);
}

} // namespace qi
#endif  // _QIMESSAGING_DETAILS_FUNCTIONTYPE_HXX_
