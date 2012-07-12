/*
**
** Copyright (C) 2012 Aldebaran Robotics
*/

#ifndef _QIMESSAGING_METAEVENT_HPP_
#define _QIMESSAGING_METAEVENT_HPP_

namespace qi {

  class MetaEventPrivate;
  class Object;
  class QIMESSAGING_API MetaEvent {
  public:
    MetaEvent(const std::string &sig);
    MetaEvent();
    MetaEvent(const MetaEvent &other);
    MetaEvent& operator=(const MetaEvent &other);
    ~MetaEvent();

    const std::string &signature() const;
    unsigned int       index() const;

    /** Event subscriber info.
     *
     * Only one of handler or target must be set.
     */
    struct Subscriber
    {
      Subscriber()
      : handler(0), target(0), method(0) {}
      Subscriber(const Functor* func)
      : handler(func), target(0), method(0) {}
      Subscriber(Object * target, unsigned int method)
      : handler(0), target(target), method(method) {}
      void call(const FunctorParameters& args);
      const Functor*     handler;
      Object*            target;
      unsigned int       method;
      /// Uid that can be passed to Object::disconnect()
      unsigned int       linkId;
    };

    /// Return a copy of all registered subscribers.
    std::vector<Subscriber> subscribers() const;
  protected:
  public:
    MetaEventPrivate   *_p;
  };

  QIMESSAGING_API qi::DataStream &operator<<(qi::DataStream &stream, const MetaEvent &meta);
  QIMESSAGING_API qi::DataStream &operator>>(qi::DataStream &stream, MetaEvent &meta);

}; // namespace qi

#endif
